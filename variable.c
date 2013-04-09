#include <string.h>
#include "vm.h"
#include "struct.h"
#include "serial.h"
#include "variable.h"
#include "util.h"

#define ERROR_VAR_TYPE  "type error"
#define VAR_MAX         1000
#define VV_SIZE         1000

const struct number_string var_types[] = {
    {VAR_NIL,   "nil"},
    {VAR_INT,   "integer"},
    {VAR_BOOL,  "boolean"},
    {VAR_FLT,   "float"},
    {VAR_STR,   "string"},
    {VAR_MAP,   "map"},
    {VAR_LST,   "list"},
    {VAR_FNC,   "function"},
    {VAR_ERR,   "error"},
    {VAR_C,     "c-function"},
};

const char *var_type_str(enum VarType vt)
{
    return NUM_TO_STRING(var_types, vt);
}

struct variable* variable_new(struct context *context, enum VarType type)
{
    null_check(context);
   if (context->num_vars++ > VAR_MAX)
        garbage_collect(context);
    struct variable* v = (struct variable*)malloc(sizeof(struct variable));
    v->type = type;
    v->map = NULL;
    v->mark = 0;
    v->visited = VISITED_NOT;
    array_add(context->all_variables, v);
    //DEBUGPRINT("variable_new %d %p\n", type, v);
    return v;
}

struct variable* variable_new_nil(struct context *context) {
    return variable_new(context, VAR_NIL);
}

struct variable* variable_new_int(struct context *context, int32_t i)
{
    struct variable *v = variable_new(context, VAR_INT);
    v->integer = i;
    return v;
}

struct variable* variable_new_err(struct context *context, const char* message)
{
    struct variable *v = variable_new(context, VAR_ERR);
    v->str = byte_array_from_string(message);
    return v;
}

struct variable* variable_new_bool(struct context *context, bool b)
{
    struct variable *v = variable_new(context, VAR_BOOL);
    v->boolean = b;
    return v;
}

void variable_del(struct context *context, struct variable *v)
{
//    DEBUGPRINT("variable_del %p\n", v);
    context->num_vars--;
    switch (v->type) {
        case VAR_C:
        case VAR_NIL:
        case VAR_INT:
        case VAR_FLT:
        case VAR_MAP:
            break;
        case VAR_SRC:
        case VAR_LST:
            array_del(v->list);
            break;
        case VAR_STR:
        case VAR_FNC:
 //           DEBUGPRINT("variable_del str %p->%s\n", v, byte_array_to_string(v->str));
            byte_array_del(v->str);
            break;
        default:
            vm_exit_message(context, "bad var type");
            break;
    }
    if (v->map != NULL) {
        struct array *keys = map_keys(v->map);
        struct array *values = map_values(v->map);
        /* for (int i=0; i<keys->length; i++) {
            byte_array_del((struct byte_array*)array_get(keys, i));
            variable_del(context, (struct variable*)array_get(values, i));
        }*/
        array_del(keys);
        array_del(values);
        map_del(v->map);
    }

    free(v);
}

struct variable *variable_new_src(struct context *context, uint32_t size)
{
    struct variable *v = variable_new(context, VAR_SRC);
    v->list = array_new();

    while (size--) {
        struct variable *o = (struct variable*)stack_pop(context->operand_stack);
        if (o->type == VAR_SRC) {
            array_append(o->list, v->list);
            v = o;
        }
        else
            array_insert(v->list, 0, o);
    }
//  DEBUGPRINT("src = %s\n", variable_value_str(context, v));
    return v;
}

struct variable *variable_new_bytes(struct context *context, struct byte_array *bytes, uint32_t size)
{
    struct variable *v = variable_new(context, VAR_BYT);
    v->str = bytes ? bytes : byte_array_new_size(size);
    return v;
}

struct variable* variable_new_float(struct context *context, float f)
{
    //DEBUGPRINT("new float %f\n", f);
    struct variable *v = variable_new(context, VAR_FLT);
    v->floater = f;
    return v;
}

struct variable *variable_new_str(struct context *context, struct byte_array *str) {
    struct variable *v = variable_new(context, VAR_STR);
    v->str = str;
//    DEBUGPRINT("variable_new_str %p->%s\n", v, byte_array_to_string(str));
    return v;
}

struct variable *variable_new_fnc(struct context *context, struct byte_array *body, struct map *closures)
{
    struct variable *v = variable_new(context, VAR_FNC);
    v->str = body;
    if (closures) {
        struct variable *vc = variable_new_map(context, closures);
        variable_map_insert(v, byte_array_from_string(RESERVED_ENV), vc);
    }
    return v;
}

struct variable *variable_new_list(struct context *context, struct array *list) {
    struct variable *v = variable_new(context, VAR_LST);

#if 0
    v->list = list ? list : array_new();
#else
    v->list = array_new();
    for (uint32_t i=0; list && (i<list->length); i++) {
        struct variable *u = (struct variable*)array_get(list, i);
        if (u->type == VAR_MAP) {
            if (v->map == NULL)
                v->map = map_new(context);
            map_update(v->map, u->map);
        } else
            array_set(v->list, v->list->length, u);
    }
#endif

    return v;
}

struct variable *variable_new_map(struct context *context, struct map *map) {
    struct variable *v = variable_new(context, VAR_MAP);
    v->map = map;
    return v;
}

struct variable *variable_new_c(struct context *context, callback2func *cfnc) {
    struct variable *v = variable_new(context, VAR_C);
    v->cfnc = cfnc;
    return v;
}

const char *variable_value_str2(struct context *context, struct variable* v, char *str, size_t size)
{
    null_check(v);
    enum VarType vt = (enum VarType)v->type;
    //char* str = (char*)malloc(VV_SIZE);
    struct array* list = v->list;

    if (v->visited ==VISITED_MORE) { // first visit of reused variable
        sprintf(str, "&%d", v->mark);
        v->visited = VISITED_X;
    }
    else if (v->visited == VISITED_X) { // subsequent visit
        sprintf(str, "*%d", v->mark);
        return str;
    }

    switch (vt) {
        case VAR_NIL:    sprintf(str, "%snil", str);                               break;
        case VAR_INT:    sprintf(str, "%s%d", str, v->integer);                    break;
        case VAR_BOOL:   sprintf(str, "%s%s", str, v->boolean ? "true" : "false"); break;
        case VAR_FLT:    sprintf(str, "%s%f", str, v->floater);                    break;
        case VAR_FNC:    sprintf(str, "%sf(%dB)", str, v->str->length);            break;
        case VAR_C:      sprintf(str, "%sc-function", str);                        break;
        case VAR_MAP:                                                              break;
        case VAR_SRC:
        case VAR_LST: {
            strcat(str, "[");
            vm_null_check(context, list);
            for (int i=0; i<list->length; i++) {
                struct variable* element = (struct variable*)array_get(list, i);
                vm_null_check(context, element);
                const char *q = (element->type == VAR_STR || element->type == VAR_FNC) ? "'" : "";
                const char *c = i ? "," : "";
                int position = strlen(str);
                char *str2 = &str[position];
                int len2 = sizeof(str) - position;
                const char *estr = variable_value_str2(context, element, str2, len2);
                sprintf(str, "%s%s%s%s%s", str, c, q, estr, q);
            }
        } break;
        case VAR_STR: {
            char *str2 =  byte_array_to_string(v->str);
            sprintf(str, "%s%s", str, str2);
            free(str2);
        } break;
        case VAR_ERR: {
            char *str2 =  byte_array_to_string(v->str);
            strcpy(str, str2);
            free(str2);
        } break;
        case VAR_BYT:
            byte_array_print(str, VV_SIZE, v->str);
            break;
        default:
            vm_exit_message(context, ERROR_VAR_TYPE);
            break;
    }

    if (v->map) {
        const struct array *a = map_keys(v->map);
        const struct array *b = map_values(v->map);

        if (vt != VAR_LST)
            strcat(str, "<");
        else if (v->list->length && a->length)
            strcat(str, ",");
        for (int i=0; i<a->length; i++) {
            if (i)
                strcat(str, ",");
            strcat(str, "'");
            char *str3 = byte_array_to_string((struct byte_array*)array_get(a,i));
            strcat(str, str3);
            free(str3);
            strcat(str, "'");
            strcat(str, ":");
            struct variable *biv = (struct variable*)array_get(b,i);
            int position = strlen(str);
            char *str2 = &str[position];
            int len2 = sizeof(str) - position;
            const char *bistr = variable_value_str2(context, biv, str2, len2);
            strcat(str, bistr);
        }
        strcat(str, vt==VAR_LST ? "]" : ">");
    }
    else if (vt == VAR_LST || vt == VAR_SRC)
        strcat(str, "]");

    return str;
}

static void variable_mark2(struct variable *v, uint32_t *marker)
{
    if (v->visited == VISITED_MORE)
        return;
    if (v->visited == VISITED_ONCE) {
        v->visited = VISITED_MORE;
        return;
    }

    // first visit
    v->mark = ++(*marker);
    v->visited = VISITED_ONCE;

    if (v->map) {
        const struct array *values = map_values(v->map);
        for (int i=0; values && i<values->length; i++)
            variable_mark2((struct variable*)array_get(values, i), marker);
    }

    if (v->type == VAR_LST) {
        for (int i=0; i<v->list->length; i++)
            variable_mark2((struct variable*)array_get(v->list, i), marker);
    }
}

void variable_mark(struct variable *v)
{
    uint32_t marker = 0;
    variable_mark2(v, &marker);
}

void variable_unmark(struct variable *v)
{
    if (v->visited == VISITED_NOT)
        return;
    v->mark = 0;
    v->visited = VISITED_NOT;
    if (v->type == VAR_LST) {
        for (int i=0; i<v->list->length; i++) {
            struct variable* element = (struct variable*)array_get(v->list, i);
            variable_unmark(element);
        }
    }
    if (v->map) {
        const struct array *a = map_keys(v->map);
        const struct array *b = map_values(v->map);
        for (int i=0; i<a->length; i++) {
            struct variable *biv = (struct variable*)array_get(b,i);
            variable_unmark(biv);
        }
    }
}


const char *variable_value_str(struct context *context, struct variable* v)
{
    char buf[1000];
    *buf = 0;
    variable_unmark(v);
    variable_mark(v);
    const char *str = variable_value_str2(context, v, buf, sizeof(buf));
    variable_unmark(v);
    return str;
}

struct byte_array *variable_value(struct context *c, struct variable *v) {
    const char *str = variable_value_str(c, v);
    return byte_array_from_string(str);
}

struct variable *variable_pop(struct context *context)
{
    struct variable *v = (struct variable*)stack_pop(context->operand_stack);
    null_check(v);
//    DEBUGPRINT("\nvariable_pop %s\n", variable_value_str(context, v));
//    print_operand_stack(context);
    if (v->type == VAR_SRC) {
//        DEBUGPRINT("\tsrc %d ", v->list->length);
        if (v->list->length)
            v = (struct variable*)array_get(v->list, 0);
        else
            v = variable_new_nil(context);
    }
    return v;
}

void variable_push(struct context *context, struct variable *v)
{
    stack_push(context->operand_stack, v);
}

struct byte_array *variable_serialize(struct context *context,
									  struct byte_array *bits,
                                      const struct variable *in,
                                      bool withType)
{
	null_check(context);
    //DEBUGPRINT("\tserialize:%s\n", variable_value_str(context, (struct variable*)in));
    if (!bits)
        bits = byte_array_new();
    if (withType)
        serial_encode_int(bits, in->type);
    switch (in->type) {
        case VAR_INT:    serial_encode_int(bits, in->integer);    break;
        case VAR_FLT:    serial_encode_float(bits, in->floater);    break;
        case VAR_STR:
        case VAR_FNC:    serial_encode_string(bits, in->str);        break;
        case VAR_LST: {
            serial_encode_int(bits, in->list->length);
            for (int i=0; i<in->list->length; i++)
                variable_serialize(context, bits, (const struct variable*)array_get(in->list, i), true);
            if (in->map) {
                const struct array *keys = map_keys(in->map);
                const struct array *values = map_values(in->map);
                serial_encode_int(bits, keys->length);
                for (int i=0; i<keys->length; i++) {
                    serial_encode_string(bits, (const struct byte_array*)array_get(keys, i));
                    variable_serialize(context, bits, (const struct variable*)array_get(values, i), true);
                }
            } else
                serial_encode_int(bits, 0);
        } break;
        default:        vm_exit_message(context, "bad var type");                break;
    }

    return bits;
}

struct variable *variable_deserialize(struct context *context, struct byte_array *bits)
{
	null_check(context);
    enum VarType vt = (enum VarType)serial_decode_int(bits);
    switch (vt) {
        case VAR_NIL:    return variable_new_nil(context);
        case VAR_INT:    return variable_new_int(context, serial_decode_int(bits));
        case VAR_FLT:    return variable_new_float(context, serial_decode_float(bits));
        case VAR_FNC:    return variable_new_fnc(context, serial_decode_string(bits), NULL);
        case VAR_STR:    return variable_new_str(context, serial_decode_string(bits));
        case VAR_LST: {
            uint32_t size = serial_decode_int(bits);
            struct array *list = array_new_size(size);
            while (size--)
                array_add(list, variable_deserialize(context, bits));
            struct variable *out = variable_new_list(context, list);

            uint32_t map_length = serial_decode_int(bits);
            if (map_length) {
                out->map = map_new();
                for (int i=0; i<map_length; i++) {
                    struct byte_array *key = serial_decode_string(bits);
                    struct variable *value = variable_deserialize(context, bits);
                    map_insert(out->map, key, value);
                }
            }
            return out;
        }
        default:
            vm_exit_message(context, "bad var type");
            return NULL;
    }
}

uint32_t variable_length(struct context *context, const struct variable *v)
{
    switch (v->type) {
        case VAR_LST: return v->list->length;
        case VAR_STR: return v->str->length;
        case VAR_INT: return v->integer;
        case VAR_NIL: return 0;
        default:
            vm_exit_message(context, "non-indexable length");
            return 0;
    }
}

struct variable *variable_part(struct context *context, struct variable *self, uint32_t start, int32_t length)
{
    null_check(self);

    if (length < 0) // count back from end of list/string
        length = self->list->length + length + 1 - start;
    if (length < 0) // end < start
        length = 0;

    switch (self->type) {
        case VAR_STR: {
            struct byte_array *str = byte_array_part(self->str, start, length);
            return variable_new_str(context, str);
        }
        case VAR_LST: {
            struct array *list = array_part(self->list, start, length);
            return variable_new_list(context, list);
        }
        default:
            return (struct variable*)exit_message("bad part type");
    }
}

void variable_remove(struct variable *self, uint32_t start, int32_t length)
{
    null_check(self);
    switch (self->type) {
        case VAR_STR:
            byte_array_remove(self->str, start, length);
            break;
        case VAR_LST:
            array_remove(self->list, start, length);
            break;
        default:
            exit_message("bad remove type");
    }
}

struct variable *variable_concatenate(struct context *context, int n, const struct variable* v, ...)
{
    struct variable* result = variable_copy(context, v);

    va_list argp;
    for(va_start(argp, v); --n;) {
        struct variable* parameter = va_arg(argp, struct variable* );
        if (!parameter)
            continue;
        else switch (result->type) {
            case VAR_STR: byte_array_append(result->str, parameter->str); break;
            case VAR_LST: array_append(result->list, parameter->list);    break;
            default: return (struct variable*)exit_message("bad concat type");
        }
    }

    va_end(argp);
    return result;
}

int variable_map_insert(struct variable* v, const struct byte_array *key, struct variable *datum)
{
    if (!v->map)
        v->map = map_new();
    return map_insert(v->map, key, datum);
}

struct variable *variable_map_get(struct context *context, const struct variable* v, const struct byte_array *key)
{
    if (!v->map)
        return variable_new_nil(context);
    return (struct variable*)map_get(v->map, key);
}

static bool variable_compare_maps(struct context *context, const struct map *umap, const struct map *vmap)
{
    if (!umap && !vmap)
        return true;
    if (!umap)
        return variable_compare_maps(context, vmap, umap);
    struct array *keys = map_keys(umap);
    if (!vmap)
        return !keys->length;
    
    for (int i=0; i<keys->length; i++) {
        struct byte_array *key = (struct byte_array*)array_get(keys, i);
        struct variable *uvalue = (struct variable*)map_get(umap, key);
        struct variable *vvalue = (struct variable*)map_get(vmap, key);
        if (!variable_compare(context, uvalue, vvalue))
            return false;
    }
    return true;
}

bool variable_compare(struct context *context, const struct variable *u, const struct variable *v)
{
    if (!u != !v)
        return false;
    enum VarType ut = (enum VarType)u->type;
    enum VarType vt = (enum VarType)v->type;
    
    if (ut != vt)
        return false;
    
    switch (ut) {
        case VAR_LST:
            if (u->list->length != v->list->length)
                return false;
            for (int i=0; i<u->list->length; i++) {
                struct variable *ui = (struct variable*)array_get(u->list, i);
                struct variable *vi = (struct variable*)array_get(v->list, i);
                if (!variable_compare(context, ui, vi))
                    return false;
            }
            break;
        case VAR_BOOL:
        case VAR_INT:   if (u->integer != v->integer)           return false; break;
        case VAR_FLT:   if (u->floater != v->floater)           return false; break;
        case VAR_STR:   if (!byte_array_equals(u->str, v->str)) return false; break;
        default:
            return (bool)vm_exit_message(context, "bad comparison");
    }
    
    return variable_compare_maps(context, u->map, v->map);
}
