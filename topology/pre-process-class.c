/*
  Copyright(c) 2021 Intel Corporation
  All rights reserved.

  This program is free software; you can redistribute it and/or modify
  it under the terms of version 2 of the GNU General Public License as
  published by the Free Software Foundation.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
  The full GNU General Public License is included in this distribution
  in the file called LICENSE.GPL.
*/
#include <assert.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <alsa/input.h>
#include <alsa/output.h>
#include <alsa/conf.h>
#include <alsa/error.h>
#include <alsa/global.h>
#include <alsa/topology.h>
#include "gettext.h"
#include "topology.h"
#include "pre-processor.h"

static struct tplg_class *tplg_class_lookup(struct tplg_pre_processor *tplg_pp, const char *name)
{
	struct tplg_class *class;

	list_for_each_entry(class, &tplg_pp->class_list, list) {
		if (!strcmp(class->name, name))
			return class;
	}

	return NULL;
}

/* save valid values references for attributes */
static int tplg_parse_constraint_valid_value_ref(struct tplg_pre_processor *tplg_pp ATTRIBUTE_UNUSED,
					      snd_config_t *cfg, struct tplg_attribute *attr)
{
	struct attribute_constraint *c = &attr->constraint;
	snd_config_iterator_t i, next;
	snd_config_t *n;

	snd_config_for_each(i, next, cfg) {
		struct tplg_attribute_ref *ref;
		const char *id, *s;
		long value;
		int err;

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0) {
			SNDERR("Invalid reference ID for '%s'\n", attr->name);
			return -EINVAL;
		}

		err = snd_config_get_string(n, &s);
		if (err < 0) {
			err = snd_config_get_integer(n, &value);
			if (err < 0) {
				SNDERR("Invalid reference value for attribute %s, must be integer",
				       attr->name);
				return err;
			}
		} else {
			if (s[0] < '0' || s[0] > '9') {
				SNDERR("Reference value not an integer for %s\n", attr->name);
				return -EINVAL;
			}
			value = atoi(s);
		}

		/* update the value ref with the tuple value */
		list_for_each_entry(ref, &c->value_list, list)
			if (!strcmp(ref->id, id)) {
				ref->value = value;
				break;
			}
	}

	return 0;
}

/* save valid values for attributes */
static int tplg_parse_constraint_valid_values(struct tplg_pre_processor *tplg_pp ATTRIBUTE_UNUSED,
					      snd_config_t *cfg, struct tplg_attribute *attr)
{
	struct attribute_constraint *c = &attr->constraint;
	snd_config_iterator_t i, next;
	snd_config_t *n;

	snd_config_for_each(i, next, cfg) {
		struct tplg_attribute_ref *ref;
		const char *id, *s;
		int err;

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0) {
			SNDERR("invalid reference value for '%s'\n", attr->name);
			return -EINVAL;
		}

		ref = calloc(1, sizeof(*ref));
		if (!ref)
			return -ENOMEM;

		err = snd_config_get_string(n, &s);
		if (err < 0) {
			SNDERR("Invalid valid value for %s\n", attr->name);
			return err;
		}

		ref->string = s;
		ref->id = id;
		ref->value = -EINVAL;
		list_add(&ref->list, &c->value_list);
	}

	return 0;
}

/*
 * Attributes can be associated with constraints such as min, max values.
 * Some attributes could also have pre-defined valid values.
 * The pre-defined values are human-readable values that sometimes need to be translated
 * to tuple values for provate data. For ex: the value "playback" and "capture" for
 * direction attributes need to be translated to 0 and 1 respectively for a DAI widget
 */
static int tplg_parse_class_constraints(struct tplg_pre_processor *tplg_pp ATTRIBUTE_UNUSED,
					snd_config_t *cfg,struct tplg_attribute *attr)
{
	struct attribute_constraint *c = &attr->constraint;
	snd_config_iterator_t i, next;
	snd_config_t *n;
	int err;

	snd_config_for_each(i, next, cfg) {
		const char *id;
		long v;

		n = snd_config_iterator_entry(i);

		if (snd_config_get_id(n, &id) < 0)
			continue;

		/* set min value constraint */
		if (!strcmp(id, "min")) {
			err = snd_config_get_integer(n, &v);
			if (err < 0) {
				SNDERR("Invalid min constraint for %s\n", attr->name);
				return err;
			}
			c->min = v;
			continue;
		}

		/* set max value constraint */
		if (!strcmp(id, "max")) {
			err = snd_config_get_integer(n, &v);
			if (err < 0) {
				SNDERR("Invalid max constraint for %s\n", attr->name);
				return err;
			}
			c->max = v;
			continue;
		}

		/* parse the list of valid values */
		if (!strcmp(id, "valid_values")) {
			err = tplg_parse_constraint_valid_values(tplg_pp, n, attr);
			if (err < 0) {
				SNDERR("Error parsing valid values for %s\n", attr->name);
				return err;
			}
			continue;
		}

		/* parse reference for string values that need to be translated to tuple values */
		if (!strcmp(id, "tuple_values")) {
			err = tplg_parse_constraint_valid_value_ref(tplg_pp, n, attr);
			if (err < 0) {
				SNDERR("Error parsing valid values for %s\n", attr->name);
				return err;
			}
		}
	}

	return 0;
}

static int tplg_parse_class_attribute(struct tplg_pre_processor *tplg_pp,
				      snd_config_t *cfg, struct tplg_attribute *attr)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *id;
	int ret;

	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);

		if (snd_config_get_id(n, &id) < 0)
			continue;

		/* Parse class attribute constraints */
		if (!strcmp(id, "constraints")) {
			ret = tplg_parse_class_constraints(tplg_pp, n, attr);
			if (ret < 0) {
				SNDERR("Error parsing constraints for %s\n", attr->name);
				return -EINVAL;
			}
			continue;
		}

		/*
		 * Parse token reference for class attributes/arguments. The token_ref field
		 * stores the name of SectionVendorTokens and type that will be used to build
		 * the tuple value for the attribute. For ex: "sof_tkn_dai.word" refers to the
		 * SectionVendorTokens with the name "sof_tkn_dai" and "word" refers to the
		 * tuple types.
		 */
		if (!strcmp(id, "token_ref")) {
			const char *s;

			if (snd_config_get_string(n, &s) < 0) {
				SNDERR("invalid token_ref for attribute %s\n", attr->name);
				return -EINVAL;
			}

			snd_strlcpy(attr->token_ref, s, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
			continue;
		}
	}

	return 0;
}

/* Parse class attributes/arguments and add to class attribute_list */
static int tplg_parse_class_attributes(struct tplg_pre_processor *tplg_pp,
				       snd_config_t *cfg, struct tplg_class *class, int type)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *id;
	int ret;

	snd_config_for_each(i, next, cfg) {
		struct tplg_attribute *attr;

		n = snd_config_iterator_entry(i);

		if (snd_config_get_id(n, &id) < 0)
			continue;

		attr = calloc(1, sizeof(*attr));
		if (!attr)
			return -ENOMEM;

		attr->param_type = type;
		if (type == TPLG_CLASS_PARAM_TYPE_ARGUMENT)
			class->num_args++;


		/* init attribute */
		INIT_LIST_HEAD(&attr->constraint.value_list);
		attr->constraint.min = INT_MIN;
		attr->constraint.max = INT_MAX;

		/* set attribute name */
		snd_strlcpy(attr->name, id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);


		ret = tplg_parse_class_attribute(tplg_pp, n, attr);
		if (ret < 0)
			return ret;

		/* add to class attribute list */
		list_add_tail(&attr->list, &class->attribute_list);
	}

	return 0;
}

/* helper function to get an attribute by name */
struct tplg_attribute *tplg_get_attribute_by_name(struct list_head *list, const char *name)
{
	struct list_head *pos;

	list_for_each(pos, list) {
		struct tplg_attribute *attr = list_entry(pos, struct tplg_attribute, list);

		if (!strcmp(attr->name, name))
			return attr;
	}

	return NULL;
}

/* apply the category mask to the all attributes */
static int tplg_parse_class_attribute_category(snd_config_t *cfg, struct tplg_class *class,
					       int category)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;

	snd_config_for_each(i, next, cfg) {
		struct tplg_attribute *attr;
		const char *id;

		n = snd_config_iterator_entry(i);
		if (snd_config_get_string(n, &id) < 0) {
			SNDERR("invalid attribute category name for class %s\n", class->name);
			return -EINVAL;
		}

		attr = tplg_get_attribute_by_name(&class->attribute_list, id);
		if (!attr)
			continue;

		attr->constraint.mask |= category;
	}

	return 0;
}

/*
 * At the end of class attribute definitions, there could be section categorizing attributes
 * as mandatory, immutable or deprecated etc. Parse these and apply them to the attribute
 * constraint.
 */
static int tplg_parse_class_attribute_categories(snd_config_t *cfg, struct tplg_class *class)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	int category = 0;
	int ret;

	snd_config_for_each(i, next, cfg) {
		const char *id;

		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0) {
			SNDERR("invalid attribute category for class %s\n", class->name);
			return -EINVAL;
		}

		if (!strcmp(id, "mandatory"))
			category = TPLG_CLASS_ATTRIBUTE_MASK_MANDATORY;

		if (!strcmp(id, "immutable"))
			category = TPLG_CLASS_ATTRIBUTE_MASK_IMMUTABLE;

		if (!strcmp(id, "deprecated"))
			category = TPLG_CLASS_ATTRIBUTE_MASK_DEPRECATED;

		if (!strcmp(id, "automatic"))
			category = TPLG_CLASS_ATTRIBUTE_MASK_AUTOMATIC;

		if (!strcmp(id, "unique")) {
			struct tplg_attribute *unique_attr;
			const char *s;
			int err = snd_config_get_string(n, &s);
			assert(err >= 0);

			unique_attr = tplg_get_attribute_by_name(&class->attribute_list, s);
			if (!unique_attr)
				continue;

			unique_attr->constraint.mask |= TPLG_CLASS_ATTRIBUTE_MASK_UNIQUE;
			continue;
		}

		if (!category)
			continue;

		/* apply the constraint to all attributes belong to the category */
		ret = tplg_parse_class_attribute_category(n, class, category);
		if (ret < 0)
			return ret;
	}

	return 0;
}

static int tplg_define_class(struct tplg_pre_processor *tplg_pp, snd_config_t *cfg, int type)
{
	snd_config_iterator_t i, next;
	struct tplg_class *class;
	snd_config_t *n;
	const char *id;
	int ret;

	if (snd_config_get_id(cfg, &id) < 0) {
		SNDERR("Invalid name for class\n");
		return -EINVAL;
	}

	/* check if the class exists already */
	class = tplg_class_lookup(tplg_pp, id);
	if (class)
		return 0;

	/* init new class */
	class = calloc(1, sizeof(struct tplg_class));
	if (!class)
		return -ENOMEM;

	snd_strlcpy(class->name, id, SNDRV_CTL_ELEM_ID_NAME_MAXLEN);
	INIT_LIST_HEAD(&class->attribute_list);
	list_add(&class->list, &tplg_pp->class_list);

	/* Parse the class definition */
	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		/* parse arguments */
		if (!strcmp(id, "DefineArgument")) {
			ret = tplg_parse_class_attributes(tplg_pp, n, class,
							  TPLG_CLASS_PARAM_TYPE_ARGUMENT);
			if (ret < 0) {
				SNDERR("failed to parse args for class %s\n", class->name);
				return ret;
			}

			continue;
		}

		/* parse attributes */
		if (!strcmp(id, "DefineAttribute")) {
			ret = tplg_parse_class_attributes(tplg_pp, n, class,
							  TPLG_CLASS_PARAM_TYPE_ATTRIBUTE);
			if (ret < 0) {
				SNDERR("failed to parse attributes for class %s\n", class->name);
				return ret;
			}
		}

		/* parse attribute constraint category and apply the constraint */
		if (!strcmp(id, "attributes")) {
			ret = tplg_parse_class_attribute_categories(n, class);
			if (ret < 0) {
				SNDERR("failed to parse attributes for class %s\n", class->name);
				return ret;
			}
			continue;
		}
	}

	tplg_pp_debug("Created class: '%s'\n", class->name);

	return 0;
}

int tplg_define_classes(struct tplg_pre_processor *tplg_pp, snd_config_t *cfg)
{
	snd_config_iterator_t i, next;
	snd_config_t *n;
	const char *id;
	int ret;

	/* create class */
	snd_config_for_each(i, next, cfg) {
		n = snd_config_iterator_entry(i);
		if (snd_config_get_id(n, &id) < 0)
			continue;

		ret = tplg_define_class(tplg_pp, n, SND_TPLG_CLASS_TYPE_BASE);
		if (ret < 0) {
			SNDERR("Failed to create class %s\n", id);
			return ret;
		}
	}

	return 0;
}
