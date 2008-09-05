/* 
   ldb database library

   Copyright (C) Andrew Tridgell  2005

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
  register handlers for specific attributes and objectclass relationships

  this allows a backend to store its schema information in any format
  it likes (or to not have any schema information at all) while keeping the 
  message matching logic generic
*/

#include "ldb_includes.h"

/*
  add a attribute to the ldb_schema

  if flags contains LDB_ATTR_FLAG_ALLOCATED
  the attribute name string will be copied using
  talloc_strdup(), otherwise it needs to be a static const
  string at least with a lifetime longer than the ldb struct!
  
  the ldb_schema_syntax structure should be a pointer
  to a static const struct or at least it needs to be
  a struct with a longer lifetime than the ldb context!

*/
int ldb_schema_attribute_add_with_syntax(struct ldb_context *ldb, 
					 const char *attribute,
					 unsigned flags,
					 const struct ldb_schema_syntax *syntax)
{
	int i, n;
	struct ldb_schema_attribute *a;

	if (!syntax) {
		return LDB_ERR_OPERATIONS_ERROR;
	}

	n = ldb->schema.num_attributes + 1;

	a = talloc_realloc(ldb, ldb->schema.attributes,
			   struct ldb_schema_attribute, n);
	if (a == NULL) {
		ldb_oom(ldb);
		return -1;
	}
	ldb->schema.attributes = a;

	for (i = 0; i < ldb->schema.num_attributes; i++) {
		int cmp = ldb_attr_cmp(attribute, a[i].name);
		if (cmp == 0) {
			/* silently ignore attempts to overwrite fixed attributes */
			if (a[i].flags & LDB_ATTR_FLAG_FIXED) {
				return 0;
			}
			if (a[i].flags & LDB_ATTR_FLAG_ALLOCATED) {
				talloc_free(discard_const_p(char, a[i].name));
			}
			/* To cancel out increment below */
			ldb->schema.num_attributes--;
			break;
		} else if (cmp < 0) {
			memmove(a+i+1, a+i, sizeof(*a) * (ldb->schema.num_attributes-i));
			break;
		}
	}
	ldb->schema.num_attributes++;

	a[i].name	= attribute;
	a[i].flags	= flags;
	a[i].syntax	= syntax;

	if (a[i].flags & LDB_ATTR_FLAG_ALLOCATED) {
		a[i].name = talloc_strdup(a, a[i].name);
		if (a[i].name == NULL) {
			ldb_oom(ldb);
			return -1;
		}
	}

	return 0;
}

static const struct ldb_schema_syntax ldb_syntax_default = {
	.name            = LDB_SYNTAX_OCTET_STRING,
	.ldif_read_fn    = ldb_handler_copy,
	.ldif_write_fn   = ldb_handler_copy,
	.canonicalise_fn = ldb_handler_copy,
	.comparison_fn   = ldb_comparison_binary
};

static const struct ldb_schema_attribute ldb_attribute_default = {
	.name	= NULL,
	.flags	= 0,
	.syntax	= &ldb_syntax_default
};

/*
  return the attribute handlers for a given attribute
*/
const struct ldb_schema_attribute *ldb_schema_attribute_by_name(struct ldb_context *ldb,
								const char *name)
{
	int i, e, b = 0, r;
	const struct ldb_schema_attribute *def = &ldb_attribute_default;

	/* as handlers are sorted, '*' must be the first if present */
	if (strcmp(ldb->schema.attributes[0].name, "*") == 0) {
		def = &ldb->schema.attributes[0];
		b = 1;
	}

	/* do a binary search on the array */
	e = ldb->schema.num_attributes - 1;

	while (b <= e) {

		i = (b + e) / 2;

		r = ldb_attr_cmp(name, ldb->schema.attributes[i].name);
		if (r == 0) {
			return &ldb->schema.attributes[i];
		}
		if (r < 0) {
			e = i - 1;
		} else {
			b = i + 1;
		}

	}

	return def;
}


/*
  add to the list of ldif handlers for this ldb context
*/
void ldb_schema_attribute_remove(struct ldb_context *ldb, const char *name)
{
	const struct ldb_schema_attribute *a;
	int i;

	a = ldb_schema_attribute_by_name(ldb, name);
	if (a == NULL || a->name == NULL) {
		return;
	}

	/* FIXED attributes are never removed */
	if (a->flags & LDB_ATTR_FLAG_FIXED) {
		return;
	}

	if (a->flags & LDB_ATTR_FLAG_ALLOCATED) {
		talloc_free(discard_const_p(char, a->name));
	}

	i = a - ldb->schema.attributes;
	if (i < ldb->schema.num_attributes - 1) {
		memmove(&ldb->schema.attributes[i], 
			a+1, sizeof(*a) * (ldb->schema.num_attributes-(i+1)));
	}

	ldb->schema.num_attributes--;
}

/*
  setup a attribute handler using a standard syntax
*/
int ldb_schema_attribute_add(struct ldb_context *ldb,
			     const char *attribute,
			     unsigned flags,
			     const char *syntax)
{
	const struct ldb_schema_syntax *s = ldb_standard_syntax_by_name(ldb, syntax);
	return ldb_schema_attribute_add_with_syntax(ldb, attribute, flags, s);
}

/*
  setup the attribute handles for well known attributes
*/
int ldb_setup_wellknown_attributes(struct ldb_context *ldb)
{
	const struct {
		const char *attr;
		const char *syntax;
	} wellknown[] = {
		{ "dn", LDB_SYNTAX_DN },
		{ "distinguishedName", LDB_SYNTAX_DN },
		{ "cn", LDB_SYNTAX_DIRECTORY_STRING },
		{ "dc", LDB_SYNTAX_DIRECTORY_STRING },
		{ "ou", LDB_SYNTAX_DIRECTORY_STRING },
		{ "objectClass", LDB_SYNTAX_OBJECTCLASS }
	};
	int i;
	int ret;

	for (i=0;i<ARRAY_SIZE(wellknown);i++) {
		ret = ldb_schema_attribute_add(ldb, wellknown[i].attr, 0,
					       wellknown[i].syntax);
		if (ret != LDB_SUCCESS) {
			return ret;
		}
	}

	return LDB_SUCCESS;
}

