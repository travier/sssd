/*
   ldb database library

   Copyright (C) Andrew Tridgell  2004
   Copyright (C) Stefan Metzmacher  2004
   Copyright (C) Simo Sorce       2006


     ** NOTE! The following LGPL license applies to the ldb
     ** library. This does NOT imply that all of Samba is released
     ** under the LGPL

   This library is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   This library is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this library; if not, see <http://www.gnu.org/licenses/>.
*/

/*
 *  Name: ldb_tdb
 *
 *  Component: ldb tdb backend
 *
 *  Description: core functions for tdb backend
 *
 *  Author: Andrew Tridgell
 *  Author: Stefan Metzmacher
 *
 *  Modifications:
 *
 *  - description: make the module use asyncronous calls
 *    date: Feb 2006
 *    Author: Simo Sorce
 */

#include "ldb_includes.h"

#include "ldb_tdb.h"


/*
  map a tdb error code to a ldb error code
*/
static int ltdb_err_map(enum TDB_ERROR tdb_code)
{
	switch (tdb_code) {
	case TDB_SUCCESS:
		return LDB_SUCCESS;
	case TDB_ERR_CORRUPT:
	case TDB_ERR_OOM:
	case TDB_ERR_EINVAL:
		return LDB_ERR_OPERATIONS_ERROR;
	case TDB_ERR_IO:
		return LDB_ERR_PROTOCOL_ERROR;
	case TDB_ERR_LOCK:
	case TDB_ERR_NOLOCK:
		return LDB_ERR_BUSY;
	case TDB_ERR_LOCK_TIMEOUT:
		return LDB_ERR_TIME_LIMIT_EXCEEDED;
	case TDB_ERR_EXISTS:
		return LDB_ERR_ENTRY_ALREADY_EXISTS;
	case TDB_ERR_NOEXIST:
		return LDB_ERR_NO_SUCH_OBJECT;
	case TDB_ERR_RDONLY:
		return LDB_ERR_INSUFFICIENT_ACCESS_RIGHTS;
	}
	return LDB_ERR_OTHER;
}


struct ldb_handle *init_ltdb_handle(struct ltdb_private *ltdb,
				    struct ldb_module *module,
				    struct ldb_request *req)
{
	struct ltdb_context *ac;
	struct ldb_handle *h;

	h = talloc_zero(req, struct ldb_handle);
	if (h == NULL) {
		ldb_set_errstring(module->ldb, "Out of Memory");
		return NULL;
	}

	h->module = module;

	ac = talloc_zero(h, struct ltdb_context);
	if (ac == NULL) {
		ldb_set_errstring(module->ldb, "Out of Memory");
		talloc_free(h);
		return NULL;
	}

	h->private_data = (void *)ac;

	h->state = LDB_ASYNC_INIT;
	h->status = LDB_SUCCESS;

	ac->module = module;
	ac->context = req->context;
	ac->callback = req->callback;

	return h;
}

/*
  form a TDB_DATA for a record key
  caller frees

  note that the key for a record can depend on whether the
  dn refers to a case sensitive index record or not
*/
struct TDB_DATA ltdb_key(struct ldb_module *module, struct ldb_dn *dn)
{
	struct ldb_context *ldb = module->ldb;
	TDB_DATA key;
	char *key_str = NULL;
	const char *dn_folded = NULL;

	/*
	  most DNs are case insensitive. The exception is index DNs for
	  case sensitive attributes

	  there are 3 cases dealt with in this code:

	  1) if the dn doesn't start with @ then uppercase the attribute
             names and the attributes values of case insensitive attributes
	  2) if the dn starts with @ then leave it alone -
	     the indexing code handles the rest
	*/

	dn_folded = ldb_dn_get_casefold(dn);
	if (!dn_folded) {
		goto failed;
	}

	key_str = talloc_strdup(ldb, "DN=");
	if (!key_str) {
		goto failed;
	}

	key_str = talloc_strdup_append_buffer(key_str, dn_folded);
	if (!key_str) {
		goto failed;
	}

	key.dptr = (uint8_t *)key_str;
	key.dsize = strlen(key_str) + 1;

	return key;

failed:
	errno = ENOMEM;
	key.dptr = NULL;
	key.dsize = 0;
	return key;
}

/*
  check special dn's have valid attributes
  currently only @ATTRIBUTES is checked
*/
int ltdb_check_special_dn(struct ldb_module *module,
			  const struct ldb_message *msg)
{
	int i, j;

	if (! ldb_dn_is_special(msg->dn) ||
	    ! ldb_dn_check_special(msg->dn, LTDB_ATTRIBUTES)) {
		return 0;
	}

	/* we have @ATTRIBUTES, let's check attributes are fine */
	/* should we check that we deny multivalued attributes ? */
	for (i = 0; i < msg->num_elements; i++) {
		for (j = 0; j < msg->elements[i].num_values; j++) {
			if (ltdb_check_at_attributes_values(&msg->elements[i].values[j]) != 0) {
				ldb_set_errstring(module->ldb, "Invalid attribute value in an @ATTRIBUTES entry");
				return LDB_ERR_INVALID_ATTRIBUTE_SYNTAX;
			}
		}
	}

	return 0;
}


/*
  we've made a modification to a dn - possibly reindex and
  update sequence number
*/
static int ltdb_modified(struct ldb_module *module, struct ldb_dn *dn)
{
	int ret = LDB_SUCCESS;

	if (ldb_dn_is_special(dn) &&
	    (ldb_dn_check_special(dn, LTDB_INDEXLIST) ||
	     ldb_dn_check_special(dn, LTDB_ATTRIBUTES)) ) {
		ret = ltdb_reindex(module);
	}

	if (ret == LDB_SUCCESS &&
	    !(ldb_dn_is_special(dn) &&
	      ldb_dn_check_special(dn, LTDB_BASEINFO)) ) {
		ret = ltdb_increase_sequence_number(module);
	}

	return ret;
}

/*
  store a record into the db
*/
int ltdb_store(struct ldb_module *module, const struct ldb_message *msg, int flgs)
{
	struct ltdb_private *ltdb =
		talloc_get_type(module->private_data, struct ltdb_private);
	TDB_DATA tdb_key, tdb_data;
	int ret;

	tdb_key = ltdb_key(module, msg->dn);
	if (!tdb_key.dptr) {
		return LDB_ERR_OTHER;
	}

	ret = ltdb_pack_data(module, msg, &tdb_data);
	if (ret == -1) {
		talloc_free(tdb_key.dptr);
		return LDB_ERR_OTHER;
	}

	ret = tdb_store(ltdb->tdb, tdb_key, tdb_data, flgs);
	if (ret == -1) {
		ret = ltdb_err_map(tdb_error(ltdb->tdb));
		goto done;
	}

	ret = ltdb_index_add(module, msg);
	if (ret != LDB_SUCCESS) {
		tdb_delete(ltdb->tdb, tdb_key);
	}

done:
	talloc_free(tdb_key.dptr);
	talloc_free(tdb_data.dptr);

	return ret;
}


static int ltdb_add_internal(struct ldb_module *module,
			     const struct ldb_message *msg)
{
	int ret;

	ret = ltdb_check_special_dn(module, msg);
	if (ret != LDB_SUCCESS) {
		return ret;
	}

	if (ltdb_cache_load(module) != 0) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	ret = ltdb_store(module, msg, TDB_INSERT);

	if (ret == LDB_ERR_ENTRY_ALREADY_EXISTS) {
		ldb_asprintf_errstring(module->ldb,
					"Entry %s already exists",
					ldb_dn_get_linearized(msg->dn));
		return ret;
	}

	if (ret == LDB_SUCCESS) {
		ret = ltdb_index_one(module, msg, 1);
		if (ret != LDB_SUCCESS) {
			return ret;
		}

		ret = ltdb_modified(module, msg->dn);
		if (ret != LDB_SUCCESS) {
			return ret;
		}
	}

	return ret;
}

/*
  add a record to the database
*/
static int ltdb_add(struct ldb_module *module, struct ldb_request *req)
{
	struct ltdb_private *ltdb;
	struct ltdb_context *ltdb_ac;
	int tret, ret = LDB_SUCCESS;

	ltdb = talloc_get_type(module->private_data, struct ltdb_private);

	if (check_critical_controls(req->controls)) {
		return LDB_ERR_UNSUPPORTED_CRITICAL_EXTENSION;
	}

	req->handle = init_ltdb_handle(ltdb, module, req);
	if (req->handle == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}
	ltdb_ac = talloc_get_type(req->handle->private_data, struct ltdb_context);

	tret = ltdb_add_internal(module, req->op.add.message);
	if (tret != LDB_SUCCESS) {
		req->handle->status = tret;
		goto done;
	}

	if (ltdb_ac->callback) {
		ret = ltdb_ac->callback(module->ldb, ltdb_ac->context, NULL);
	}
done:
	req->handle->state = LDB_ASYNC_DONE;
	return ret;
}

/*
  delete a record from the database, not updating indexes (used for deleting
  index records)
*/
int ltdb_delete_noindex(struct ldb_module *module, struct ldb_dn *dn)
{
	struct ltdb_private *ltdb =
		talloc_get_type(module->private_data, struct ltdb_private);
	TDB_DATA tdb_key;
	int ret;

	tdb_key = ltdb_key(module, dn);
	if (!tdb_key.dptr) {
		return LDB_ERR_OTHER;
	}

	ret = tdb_delete(ltdb->tdb, tdb_key);
	talloc_free(tdb_key.dptr);

	if (ret != 0) {
		ret = ltdb_err_map(tdb_error(ltdb->tdb));
	}

	return ret;
}

static int ltdb_delete_internal(struct ldb_module *module, struct ldb_dn *dn)
{
	struct ldb_message *msg;
	int ret;

	msg = talloc(module, struct ldb_message);
	if (msg == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	/* in case any attribute of the message was indexed, we need
	   to fetch the old record */
	ret = ltdb_search_dn1(module, dn, msg);
	if (ret != LDB_SUCCESS) {
		/* not finding the old record is an error */
		goto done;
	}

	ret = ltdb_delete_noindex(module, dn);
	if (ret != LDB_SUCCESS) {
		goto done;
	}

	/* remove one level attribute */
	ret = ltdb_index_one(module, msg, 0);
	if (ret != LDB_SUCCESS) {
		goto done;
	}

	/* remove any indexed attributes */
	ret = ltdb_index_del(module, msg);
	if (ret != LDB_SUCCESS) {
		goto done;
	}

	ret = ltdb_modified(module, dn);
	if (ret != LDB_SUCCESS) {
		goto done;
	}

done:
	talloc_free(msg);
	return ret;
}

/*
  delete a record from the database
*/
static int ltdb_delete(struct ldb_module *module, struct ldb_request *req)
{
	struct ltdb_private *ltdb;
	struct ltdb_context *ltdb_ac;
	int tret, ret = LDB_SUCCESS;

	ltdb = talloc_get_type(module->private_data, struct ltdb_private);

	if (check_critical_controls(req->controls)) {
		return LDB_ERR_UNSUPPORTED_CRITICAL_EXTENSION;
	}

	req->handle = NULL;

	if (ltdb_cache_load(module) != 0) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	req->handle = init_ltdb_handle(ltdb, module, req);
	if (req->handle == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}
	ltdb_ac = talloc_get_type(req->handle->private_data, struct ltdb_context);

	tret = ltdb_delete_internal(module, req->op.del.dn);
	if (tret != LDB_SUCCESS) {
		req->handle->status = tret;
		goto done;
	}

	if (ltdb_ac->callback) {
		ret = ltdb_ac->callback(module->ldb, ltdb_ac->context, NULL);
	}
done:
	req->handle->state = LDB_ASYNC_DONE;
	return ret;
}

/*
  find an element by attribute name. At the moment this does a linear search,
  it should be re-coded to use a binary search once all places that modify
  records guarantee sorted order

  return the index of the first matching element if found, otherwise -1
*/
static int find_element(const struct ldb_message *msg, const char *name)
{
	unsigned int i;
	for (i=0;i<msg->num_elements;i++) {
		if (ldb_attr_cmp(msg->elements[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}


/*
  add an element to an existing record. Assumes a elements array that we
  can call re-alloc on, and assumed that we can re-use the data pointers from
  the passed in additional values. Use with care!

  returns 0 on success, -1 on failure (and sets errno)
*/
static int msg_add_element(struct ldb_context *ldb,
			   struct ldb_message *msg,
			   struct ldb_message_element *el)
{
	struct ldb_message_element *e2;
	unsigned int i;

	e2 = talloc_realloc(msg, msg->elements, struct ldb_message_element,
			      msg->num_elements+1);
	if (!e2) {
		errno = ENOMEM;
		return -1;
	}

	msg->elements = e2;

	e2 = &msg->elements[msg->num_elements];

	e2->name = el->name;
	e2->flags = el->flags;
	e2->values = NULL;
	if (el->num_values != 0) {
		e2->values = talloc_array(msg->elements,
					  struct ldb_val, el->num_values);
		if (!e2->values) {
			errno = ENOMEM;
			return -1;
		}
	}
	for (i=0;i<el->num_values;i++) {
		e2->values[i] = el->values[i];
	}
	e2->num_values = el->num_values;

	msg->num_elements++;

	return 0;
}

/*
  delete all elements having a specified attribute name
*/
static int msg_delete_attribute(struct ldb_module *module,
				struct ldb_context *ldb,
				struct ldb_message *msg, const char *name)
{
	const char *dn;
	unsigned int i, j;

	dn = ldb_dn_get_linearized(msg->dn);
	if (dn == NULL) {
		return -1;
	}

	for (i=0;i<msg->num_elements;i++) {
		if (ldb_attr_cmp(msg->elements[i].name, name) == 0) {
			for (j=0;j<msg->elements[i].num_values;j++) {
				ltdb_index_del_value(module, dn,
						     &msg->elements[i], j);
			}
			talloc_free(msg->elements[i].values);
			if (msg->num_elements > (i+1)) {
				memmove(&msg->elements[i],
					&msg->elements[i+1],
					sizeof(struct ldb_message_element)*
					(msg->num_elements - (i+1)));
			}
			msg->num_elements--;
			i--;
			msg->elements = talloc_realloc(msg, msg->elements,
							struct ldb_message_element,
							msg->num_elements);
		}
	}

	return 0;
}

/*
  delete all elements matching an attribute name/value

  return 0 on success, -1 on failure
*/
static int msg_delete_element(struct ldb_module *module,
			      struct ldb_message *msg,
			      const char *name,
			      const struct ldb_val *val)
{
	struct ldb_context *ldb = module->ldb;
	unsigned int i;
	int found;
	struct ldb_message_element *el;
	const struct ldb_schema_attribute *a;

	found = find_element(msg, name);
	if (found == -1) {
		return -1;
	}

	el = &msg->elements[found];

	a = ldb_schema_attribute_by_name(ldb, el->name);

	for (i=0;i<el->num_values;i++) {
		if (a->syntax->comparison_fn(ldb, ldb,
						&el->values[i], val) == 0) {
			if (i<el->num_values-1) {
				memmove(&el->values[i], &el->values[i+1],
					sizeof(el->values[i])*
						(el->num_values-(i+1)));
			}
			el->num_values--;
			if (el->num_values == 0) {
				return msg_delete_attribute(module, ldb,
							    msg, name);
			}
			return 0;
		}
	}

	return -1;
}


/*
  modify a record - internal interface

  yuck - this is O(n^2). Luckily n is usually small so we probably
  get away with it, but if we ever have really large attribute lists
  then we'll need to look at this again
*/
int ltdb_modify_internal(struct ldb_module *module,
			 const struct ldb_message *msg)
{
	struct ldb_context *ldb = module->ldb;
	struct ltdb_private *ltdb =
		talloc_get_type(module->private_data, struct ltdb_private);
	TDB_DATA tdb_key, tdb_data;
	struct ldb_message *msg2;
	unsigned i, j;
	int ret, idx;

	tdb_key = ltdb_key(module, msg->dn);
	if (!tdb_key.dptr) {
		return LDB_ERR_OTHER;
	}

	tdb_data = tdb_fetch(ltdb->tdb, tdb_key);
	if (!tdb_data.dptr) {
		talloc_free(tdb_key.dptr);
		return ltdb_err_map(tdb_error(ltdb->tdb));
	}

	msg2 = talloc(tdb_key.dptr, struct ldb_message);
	if (msg2 == NULL) {
		talloc_free(tdb_key.dptr);
		return LDB_ERR_OTHER;
	}

	ret = ltdb_unpack_data(module, &tdb_data, msg2);
	if (ret == -1) {
		ret = LDB_ERR_OTHER;
		goto failed;
	}

	if (!msg2->dn) {
		msg2->dn = msg->dn;
	}

	for (i=0;i<msg->num_elements;i++) {
		struct ldb_message_element *el = &msg->elements[i];
		struct ldb_message_element *el2;
		struct ldb_val *vals;
		const char *dn;

		switch (msg->elements[i].flags & LDB_FLAG_MOD_MASK) {

		case LDB_FLAG_MOD_ADD:
			/* add this element to the message. fail if it
			   already exists */
			idx = find_element(msg2, el->name);

			if (idx == -1) {
				if (msg_add_element(ldb, msg2, el) != 0) {
					ret = LDB_ERR_OTHER;
					goto failed;
				}
				continue;
			}

			el2 = &msg2->elements[idx];

			/* An attribute with this name already exists,
			 * add all values if they don't already exist
			 * (check both the other elements to be added,
			 * and those already in the db). */

			for (j=0;j<el->num_values;j++) {
				if (ldb_msg_find_val(el2, &el->values[j])) {
					ldb_asprintf_errstring(module->ldb, "%s: value #%d already exists", el->name, j);
					ret = LDB_ERR_ATTRIBUTE_OR_VALUE_EXISTS;
					goto failed;
				}
				if (ldb_msg_find_val(el, &el->values[j]) != &el->values[j]) {
					ldb_asprintf_errstring(module->ldb, "%s: value #%d provided more than once", el->name, j);
					ret = LDB_ERR_ATTRIBUTE_OR_VALUE_EXISTS;
					goto failed;
				}
			}

		        vals = talloc_realloc(msg2->elements, el2->values, struct ldb_val,
						el2->num_values + el->num_values);

			if (vals == NULL) {
				ret = LDB_ERR_OTHER;
				goto failed;
			}

			for (j=0;j<el->num_values;j++) {
				vals[el2->num_values + j] =
					ldb_val_dup(vals, &el->values[j]);
			}

			el2->values = vals;
			el2->num_values += el->num_values;

			break;

		case LDB_FLAG_MOD_REPLACE:
			/* replace all elements of this attribute name with the elements
			   listed. The attribute not existing is not an error */
			msg_delete_attribute(module, ldb, msg2, el->name);

			for (j=0;j<el->num_values;j++) {
				if (ldb_msg_find_val(el, &el->values[j]) != &el->values[j]) {
					ldb_asprintf_errstring(module->ldb, "%s: value #%d provided more than once", el->name, j);
					ret = LDB_ERR_ATTRIBUTE_OR_VALUE_EXISTS;
					goto failed;
				}
			}

			/* add the replacement element, if not empty */
			if (el->num_values != 0 &&
			    msg_add_element(ldb, msg2, el) != 0) {
				ret = LDB_ERR_OTHER;
				goto failed;
			}
			break;

		case LDB_FLAG_MOD_DELETE:

			dn = ldb_dn_get_linearized(msg->dn);
			if (dn == NULL) {
				ret = LDB_ERR_OTHER;
				goto failed;
			}

			/* we could be being asked to delete all
			   values or just some values */
			if (msg->elements[i].num_values == 0) {
				if (msg_delete_attribute(module, ldb, msg2, 
							 msg->elements[i].name) != 0) {
					ldb_asprintf_errstring(module->ldb, "No such attribute: %s for delete on %s", msg->elements[i].name, dn);
					ret = LDB_ERR_NO_SUCH_ATTRIBUTE;
					goto failed;
				}
				break;
			}
			for (j=0;j<msg->elements[i].num_values;j++) {
				if (msg_delete_element(module,
						       msg2, 
						       msg->elements[i].name,
						       &msg->elements[i].values[j]) != 0) {
					ldb_asprintf_errstring(module->ldb, "No matching attribute value when deleting attribute: %s on %s", msg->elements[i].name, dn);
					ret = LDB_ERR_NO_SUCH_ATTRIBUTE;
					goto failed;
				}
				ret = ltdb_index_del_value(module, dn, &msg->elements[i], j);
				if (ret != LDB_SUCCESS) {
					goto failed;
				}
			}
			break;
		default:
			ldb_asprintf_errstring(module->ldb,
				"Invalid ldb_modify flags on %s: 0x%x",
				msg->elements[i].name,
				msg->elements[i].flags & LDB_FLAG_MOD_MASK);
			ret = LDB_ERR_PROTOCOL_ERROR;
			goto failed;
		}
	}

	/* we've made all the mods
	 * save the modified record back into the database */
	ret = ltdb_store(module, msg2, TDB_MODIFY);
	if (ret != LDB_SUCCESS) {
		goto failed;
	}

	ret = ltdb_modified(module, msg->dn);
	if (ret != LDB_SUCCESS) {
		goto failed;
	}

	talloc_free(tdb_key.dptr);
	free(tdb_data.dptr);
	return ret;

failed:
	talloc_free(tdb_key.dptr);
	free(tdb_data.dptr);
	return ret;
}

/*
  modify a record
*/
static int ltdb_modify(struct ldb_module *module, struct ldb_request *req)
{
	struct ltdb_private *ltdb;
	struct ltdb_context *ltdb_ac;
	int tret, ret = LDB_SUCCESS;

	ltdb = talloc_get_type(module->private_data, struct ltdb_private);

	if (check_critical_controls(req->controls)) {
		return LDB_ERR_UNSUPPORTED_CRITICAL_EXTENSION;
	}

	req->handle = NULL;

	req->handle = init_ltdb_handle(ltdb, module, req);
	if (req->handle == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}
	ltdb_ac = talloc_get_type(req->handle->private_data, struct ltdb_context);

	tret = ltdb_check_special_dn(module, req->op.mod.message);
	if (tret != LDB_SUCCESS) {
		req->handle->status = tret;
		goto done;
	}

	if (ltdb_cache_load(module) != 0) {
		ret = LDB_ERR_OPERATIONS_ERROR;
		goto done;
	}

	tret = ltdb_modify_internal(module, req->op.mod.message);
	if (tret != LDB_SUCCESS) {
		req->handle->status = tret;
		goto done;
	}

	if (ltdb_ac->callback) {
		ret = ltdb_ac->callback(module->ldb, ltdb_ac->context, NULL);
	}
done:
	req->handle->state = LDB_ASYNC_DONE;
	return ret;
}

/*
  rename a record
*/
static int ltdb_rename(struct ldb_module *module, struct ldb_request *req)
{
	struct ltdb_private *ltdb;
	struct ltdb_context *ltdb_ac;
	struct ldb_message *msg;
	int tret, ret = LDB_SUCCESS;

	ltdb = talloc_get_type(module->private_data, struct ltdb_private);

	if (check_critical_controls(req->controls)) {
		return LDB_ERR_UNSUPPORTED_CRITICAL_EXTENSION;
	}

	req->handle = NULL;

	if (ltdb_cache_load(module) != 0) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	req->handle = init_ltdb_handle(ltdb, module, req);
	if (req->handle == NULL) {
		return LDB_ERR_OPERATIONS_ERROR;
	}
	ltdb_ac = talloc_get_type(req->handle->private_data, struct ltdb_context);

	msg = talloc(ltdb_ac, struct ldb_message);
	if (msg == NULL) {
		ret = LDB_ERR_OPERATIONS_ERROR;
		goto done;
	}

	/* in case any attribute of the message was indexed, we need
	   to fetch the old record */
	tret = ltdb_search_dn1(module, req->op.rename.olddn, msg);
	if (tret != LDB_SUCCESS) {
		/* not finding the old record is an error */
		req->handle->status = tret;
		goto done;
	}

	msg->dn = ldb_dn_copy(msg, req->op.rename.newdn);
	if (!msg->dn) {
		ret = LDB_ERR_OPERATIONS_ERROR;
		goto done;
	}

	if (ldb_dn_compare(req->op.rename.olddn, req->op.rename.newdn) == 0) {
		/* The rename operation is apparently only changing case -
		   the DNs are the same.  Delete the old DN before adding
		   the new one to avoid a TDB_ERR_EXISTS error.

		   The only drawback to this is that if the delete
		   succeeds but the add fails, we rely on the
		   transaction to roll this all back. */
		ret = ltdb_delete_internal(module, req->op.rename.olddn);
		if (ret != LDB_SUCCESS) {
			goto done;
		}

		ret = ltdb_add_internal(module, msg);
		if (ret != LDB_SUCCESS) {
			goto done;
		}
	} else {
		/* The rename operation is changing DNs.  Try to add the new
		   DN first to avoid clobbering another DN not related to
		   this rename operation. */
		ret = ltdb_add_internal(module, msg);
		if (ret != LDB_SUCCESS) {
			goto done;
		}

		tret = ltdb_delete_internal(module, req->op.rename.olddn);
		if (tret != LDB_SUCCESS) {
			ltdb_delete_internal(module, req->op.rename.newdn);
			ret = LDB_ERR_OPERATIONS_ERROR;
			goto done;
		}
	}

	if (ltdb_ac->callback) {
		ret = ltdb_ac->callback(module->ldb, ltdb_ac->context, NULL);
	}
done:
	req->handle->state = LDB_ASYNC_DONE;
	return ret;
}

static int ltdb_start_trans(struct ldb_module *module)
{
	struct ltdb_private *ltdb =
		talloc_get_type(module->private_data, struct ltdb_private);

	if (tdb_transaction_start(ltdb->tdb) != 0) {
		return ltdb_err_map(tdb_error(ltdb->tdb));
	}

	ltdb->in_transaction++;

	return LDB_SUCCESS;
}

static int ltdb_end_trans(struct ldb_module *module)
{
	struct ltdb_private *ltdb =
		talloc_get_type(module->private_data, struct ltdb_private);

	ltdb->in_transaction--;

	if (tdb_transaction_commit(ltdb->tdb) != 0) {
		return ltdb_err_map(tdb_error(ltdb->tdb));
	}

	return LDB_SUCCESS;
}

static int ltdb_del_trans(struct ldb_module *module)
{
	struct ltdb_private *ltdb =
		talloc_get_type(module->private_data, struct ltdb_private);

	ltdb->in_transaction--;

	if (tdb_transaction_cancel(ltdb->tdb) != 0) {
		return ltdb_err_map(tdb_error(ltdb->tdb));
	}

	return LDB_SUCCESS;
}

static int ltdb_wait(struct ldb_handle *handle, enum ldb_wait_type type)
{
	return handle->status;
}

static int ltdb_request(struct ldb_module *module, struct ldb_request *req)
{
	/* check for oustanding critical controls
	 * and return an error if found */
	if (check_critical_controls(req->controls)) {
		return LDB_ERR_UNSUPPORTED_CRITICAL_EXTENSION;
	}

	/* search, add, modify, delete, rename are handled by their own,
	 * no other op supported */
	return LDB_ERR_OPERATIONS_ERROR;
}

/*
  return sequenceNumber from @BASEINFO
*/
static int ltdb_sequence_number(struct ldb_module *module,
				struct ldb_request *req)
{
	TALLOC_CTX *tmp_ctx;
	struct ldb_message *msg = NULL;
	struct ldb_dn *dn;
	const char *date;
	int tret;

	tmp_ctx = talloc_new(req);
	if (tmp_ctx == NULL) {
		talloc_free(tmp_ctx);
		return LDB_ERR_OPERATIONS_ERROR;
	}

	dn = ldb_dn_new(tmp_ctx, module->ldb, LTDB_BASEINFO);

	msg = talloc(tmp_ctx, struct ldb_message);
	if (msg == NULL) {
		talloc_free(tmp_ctx);
		return LDB_ERR_OPERATIONS_ERROR;
	}

	req->op.seq_num.flags = 0;

	tret = ltdb_search_dn1(module, dn, msg);
	if (tret != LDB_SUCCESS) {
		talloc_free(tmp_ctx);
		/* zero is as good as anything when we don't know */
		req->op.seq_num.seq_num = 0;
		return LDB_SUCCESS;
	}

	switch (req->op.seq_num.type) {
	case LDB_SEQ_HIGHEST_SEQ:
		req->op.seq_num.seq_num = ldb_msg_find_attr_as_uint64(msg, LTDB_SEQUENCE_NUMBER, 0);
		break;
	case LDB_SEQ_NEXT:
		req->op.seq_num.seq_num = ldb_msg_find_attr_as_uint64(msg, LTDB_SEQUENCE_NUMBER, 0);
		req->op.seq_num.seq_num++;
		break;
	case LDB_SEQ_HIGHEST_TIMESTAMP:
		date = ldb_msg_find_attr_as_string(msg, LTDB_MOD_TIMESTAMP, NULL);
		if (date) {
			req->op.seq_num.seq_num = ldb_string_to_time(date);
		} else {
			req->op.seq_num.seq_num = 0;
			/* zero is as good as anything when we don't know */
		}
		break;
	}
	talloc_free(tmp_ctx);
	return LDB_SUCCESS;
}

static const struct ldb_module_ops ltdb_ops = {
	.name              = "tdb",
	.search            = ltdb_search,
	.add               = ltdb_add,
	.modify            = ltdb_modify,
	.del               = ltdb_delete,
	.rename            = ltdb_rename,
	.request           = ltdb_request,
	.start_transaction = ltdb_start_trans,
	.end_transaction   = ltdb_end_trans,
	.del_transaction   = ltdb_del_trans,
	.wait              = ltdb_wait,
	.sequence_number   = ltdb_sequence_number
};

/*
  connect to the database
*/
static int ltdb_connect(struct ldb_context *ldb, const char *url,
			unsigned int flags, const char *options[],
			struct ldb_module **module)
{
	const char *path;
	int tdb_flags, open_flags;
	struct ltdb_private *ltdb;

	/* parse the url */
	if (strchr(url, ':')) {
		if (strncmp(url, "tdb://", 6) != 0) {
			ldb_debug(ldb, LDB_DEBUG_ERROR,
				  "Invalid tdb URL '%s'", url);
			return -1;
		}
		path = url+6;
	} else {
		path = url;
	}

	tdb_flags = TDB_DEFAULT | TDB_SEQNUM;

	/* check for the 'nosync' option */
	if (flags & LDB_FLG_NOSYNC) {
		tdb_flags |= TDB_NOSYNC;
	}

	/* and nommap option */
	if (flags & LDB_FLG_NOMMAP) {
		tdb_flags |= TDB_NOMMAP;
	}

	if (flags & LDB_FLG_RDONLY) {
		open_flags = O_RDONLY;
	} else {
		open_flags = O_CREAT | O_RDWR;
	}

	ltdb = talloc_zero(ldb, struct ltdb_private);
	if (!ltdb) {
		ldb_oom(ldb);
		return -1;
	}

	/* note that we use quite a large default hash size */
	ltdb->tdb = ltdb_wrap_open(ltdb, path, 10000,
				   tdb_flags, open_flags,
				   ldb->create_perms, ldb);
	if (!ltdb->tdb) {
		ldb_debug(ldb, LDB_DEBUG_ERROR,
			  "Unable to open tdb '%s'\n", path);
		talloc_free(ltdb);
		return -1;
	}

	ltdb->sequence_number = 0;

	*module = talloc(ldb, struct ldb_module);
	if (!module) {
		ldb_oom(ldb);
		talloc_free(ltdb);
		return -1;
	}
	talloc_set_name_const(*module, "ldb_tdb backend");
	(*module)->ldb = ldb;
	(*module)->prev = (*module)->next = NULL;
	(*module)->private_data = ltdb;
	(*module)->ops = &ltdb_ops;

	if (ltdb_cache_load(*module) != 0) {
		talloc_free(*module);
		talloc_free(ltdb);
		return -1;
	}

	return 0;
}

const struct ldb_backend_ops ldb_tdb_backend_ops = {
	.name = "tdb",
	.connect_fn = ltdb_connect
};
