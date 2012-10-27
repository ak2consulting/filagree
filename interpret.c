//
//  interpret.c
//  filagree
//

#include "vm.h"
#include "compile.h"
#include "interpret.h"

#define FG_MAX_INPUT     256
#define ERROR_USAGE    "usage: filagree [file]"

void yield(struct context *context) {
    struct variable *args = stack_pop(context->operand_stack);
	struct variable *f = array_get(args->list, 1);
	struct byte_array *result = f->str;
	byte_array_append(result, byte_array_from_string(": "));
    for (int i=1; i<args->list->length; i++) {
        struct variable *v = array_get(args->list, i);
		byte_array_append(result, variable_value(context, v));
	}
	DEBUGPRINT("%s\n", byte_array_to_string(result));
}

struct variable *repl()
{
    char stdinput[FG_MAX_INPUT];
    struct variable *v = NULL;
    struct context *context = context_new();
	
    for (;;) {
        fflush(stdin);
        stdinput[0] = 0;
        if (!fgets(stdinput, FG_MAX_INPUT, stdin)) {
            if (feof(stdin))
                return 0;
            if (ferror(stdin))
                //return errno;
                return variable_new_err(context, "unknown error reading stdin");
        }
        if ((v = interpret_string(stdinput, context->find)))
            return v;
    }
}

struct variable *interpret_file(const struct byte_array *filename, find_c_var *find)
{
    struct byte_array *program = build_file(filename);
    return execute(program, false, find);
}

struct variable *execute_file(const struct byte_array* filename, find_c_var *find)
{
    struct byte_array *program = read_file(filename);
    return execute(program, false, find);
}

struct variable *run_file(const char* str, find_c_var *find, struct map *env)
{
    struct byte_array *filename = byte_array_from_string(str);
    struct byte_array *dotfgbc = byte_array_from_string(EXTENSION_BC);
    int fgbc = byte_array_find(filename, dotfgbc, 0);
    if (fgbc > 0)
        return execute_file(filename, find);
    struct byte_array *dotfg = byte_array_from_string(EXTENSION_SRC);
    int fg = byte_array_find(filename, dotfg, 0);
    if (fg > 0)
        return interpret_file(filename, find);
    return (struct variable*)exit_message("invalid file name");
}

struct variable *interpret_string(const char *str, find_c_var *find)
{
    struct variable *e;
    struct byte_array *input = byte_array_from_string(str);
    struct byte_array *program = build_string(input);
    if (program && (e = execute(program, true, find)) && (e->type == VAR_ERR))
        printf("%s\n", byte_array_to_string(e->str));
    return NULL;
}

#ifdef CLI
int main (int argc, char** argv)
{
    struct variable *v = NULL;
    switch (argc) {
        case 1:     v = repl();                         break;
        case 2:     v = run_file(argv[1], NULL, NULL);  break;
        case 3:     compile_file(argv[1]);              break;
        default:    exit_message(ERROR_USAGE);          break;
    }
    if (v && v->type==VAR_ERR)
        log_print("%s\n", variable_value_str(NULL, v));
    return v && v->type == VAR_ERR;
}
#endif // EXECUTABLE

