import json
import argparse
import os
import re

# --- Helper Functions (same as previous "exit on error" version) ---
def to_camel_case(snake_str):
    if not snake_str: return ""
    return "".join(x.capitalize() or "_" for x in snake_str.split("_"))

def sanitize_c_identifier(name):
    name = re.sub(r'[^a-zA-Z0-9_]', '_', name)
    if name and name[0].isdigit(): name = '_' + name
    if not name: name = "_unnamed_field"
    return name

def get_c_type_info(value, key_name, struct_name_prefix, structs_to_generate_queue, processed_struct_names):
    info = {
        'field_c_type': "void*", 'is_array': False, 'array_element_c_type': None,
        'array_element_is_pointer': False, 'is_single_struct_pointer': False,
        'nested_struct_def_name': None
    }
    if isinstance(value, str):
        info['field_c_type'] = "char*"
    elif isinstance(value, bool):
        info['field_c_type'] = "int"
    elif isinstance(value, int):
        info['field_c_type'] = "int"
    elif isinstance(value, float):
        info['field_c_type'] = "double"
    elif isinstance(value, dict):
        struct_name = f"{struct_name_prefix}{to_camel_case(sanitize_c_identifier(key_name))}"
        info['field_c_type'] = f"struct {struct_name}*"
        info['is_single_struct_pointer'] = True
        info['nested_struct_def_name'] = struct_name
        if struct_name not in processed_struct_names:
            structs_to_generate_queue[struct_name] = value
            processed_struct_names.add(struct_name)
    elif isinstance(value, list):
        info['is_array'] = True
        if not value:
            info['field_c_type'] = "void**"; info['array_element_c_type'] = "void*"; info['array_element_is_pointer'] = True
        else:
            first_element = value[0]
            if isinstance(first_element, str):
                info['field_c_type'] = "char**"; info['array_element_c_type'] = "char*"; info['array_element_is_pointer'] = True
            elif isinstance(first_element, int):
                info['field_c_type'] = "int*"; info['array_element_c_type'] = "int"
            elif isinstance(first_element, float):
                info['field_c_type'] = "double*"; info['array_element_c_type'] = "double"
            elif isinstance(first_element, bool):
                info['field_c_type'] = "int*"; info['array_element_c_type'] = "int"
            elif isinstance(first_element, dict):
                singular_key = key_name[:-1] if key_name.endswith('s') else key_name + "Item"
                element_struct_name = f"{struct_name_prefix}{to_camel_case(sanitize_c_identifier(singular_key))}"
                info['field_c_type'] = f"struct {element_struct_name}*"; info['array_element_c_type'] = f"struct {element_struct_name}"; info['array_element_is_pointer'] = False
                info['nested_struct_def_name'] = element_struct_name
                if element_struct_name not in processed_struct_names:
                    structs_to_generate_queue[element_struct_name] = first_element
                    processed_struct_names.add(element_struct_name)
            else:
                info['field_c_type'] = "void**"; info['array_element_c_type'] = "void*"; info['array_element_is_pointer'] = True
    elif value is None:
        info['field_c_type'] = "void*"
    return info

# --- Header Generation (MODIFIED) ---
def generate_header_file(base_name, root_struct_name, all_struct_definitions):
    h_content = [f"#ifndef {base_name.upper()}_H", f"#define {base_name.upper()}_H\n"]
    h_content.append("#include <stddef.h> // For size_t (still useful for general C)\n") # size_t might still be used by stb_ds or jansson
    # stb_ds.h itself might need stddef.h for size_t for arrlen() return type, etc.
    # So keeping stddef.h is good.

    for struct_name in all_struct_definitions.keys():
        h_content.append(f"struct {struct_name};")
    h_content.append("\n// Struct Definitions")

    for struct_name, fields in all_struct_definitions.items():
        h_content.append(f"typedef struct {struct_name} {{")
        for field_name, field_info in fields.items():
            h_content.append(f"    {field_info['field_c_type']} {field_name};")
            # REMOVED: if field_info['is_array']:
            # REMOVED:     h_content.append(f"    size_t {field_name}_count;")
        h_content.append(f"}} {struct_name};\n")

    h_content.append("\n// Parsing Function Declaration")
    h_content.append(f"{root_struct_name}* parse_{root_struct_name.lower()}_from_string(const char* json_string);")
    h_content.append(f"{root_struct_name}* parse_{root_struct_name.lower()}_from_file(const char* filename);\n")

    h_content.append("// Freeing Function Declarations")
    for struct_name in all_struct_definitions.keys():
         h_content.append(f"void free_{struct_name.lower()}({struct_name}* data);")

    h_content.append(f"\n#endif // {base_name.upper()}_H")
    return "\n".join(h_content)

# --- C File Generation (MODIFIED) ---
def generate_c_file(base_name, root_struct_name, all_struct_definitions):
    c_content = [
        f"#include \"{base_name}.h\"",
    ]

    c_content.append("// --- Forward Declarations ---")
    for struct_name in all_struct_definitions.keys():
        c_content.append(f"static void cleanup_members_{struct_name.lower()}({struct_name}* data);")
        if struct_name != root_struct_name:
             c_content.append(f"static void parse_json_to_{struct_name.lower()}(json_t* json_obj, {struct_name}* out_struct);")
    c_content.append("\n")

    c_content.append("// --- Cleanup and Freeing Functions ---")
    for struct_name in reversed(list(all_struct_definitions.keys())):
        fields = all_struct_definitions[struct_name]
        c_content.append(f"static void cleanup_members_{struct_name.lower()}({struct_name}* data) {{")
        c_content.append("    if (!data) return;")
        for field_name, field_info in fields.items():
            if field_info['is_array']:
                c_content.append(f"    // Cleanup array: {field_name} (using arrlen)")
                c_content.append(f"    if (data->{field_name}) {{") # Check if array pointer is not NULL
                c_content.append(f"        size_t count = arrlen(data->{field_name});") # Get count using arrlen
                if field_info['array_element_c_type'] == "char*" and field_info['array_element_is_pointer']:
                    c_content.append(f"        for (size_t i = 0; i < count; ++i) {{")
                    c_content.append(f"            if (data->{field_name}[i]) free(data->{field_name}[i]);")
                    c_content.append(f"        }}")
                elif field_info['array_element_c_type'].startswith("struct ") and \
                     not field_info['array_element_is_pointer']:
                    element_struct_actual_name = field_info['nested_struct_def_name']
                    c_content.append(f"        for (size_t i = 0; i < count; ++i) {{")
                    c_content.append(f"            cleanup_members_{element_struct_actual_name.lower()}(&data->{field_name}[i]);")
                    c_content.append(f"        }}")
                c_content.append(f"        arrfree(data->{field_name});")
                c_content.append(f"        data->{field_name} = NULL;")
                # REMOVED: data->{field_name}_count = 0;
                c_content.append(f"    }}")
            elif field_info['field_c_type'] == "char*":
                c_content.append(f"    if (data->{field_name}) {{ free(data->{field_name}); data->{field_name} = NULL; }}")
            elif field_info['is_single_struct_pointer']:
                nested_struct_actual_name = field_info['nested_struct_def_name']
                c_content.append(f"    if (data->{field_name}) {{ free_{nested_struct_actual_name.lower()}(data->{field_name}); data->{field_name} = NULL; }}")
        c_content.append("}\n")

        c_content.append(f"void free_{struct_name.lower()}({struct_name}* data) {{")
        c_content.append("    if (!data) return;")
        c_content.append(f"    cleanup_members_{struct_name.lower()}(data);")
        c_content.append("    free(data);")
        c_content.append("}\n")

    c_content.append("\n// --- Parsing Helper Functions (exit on error) ---")
    for struct_name, fields in all_struct_definitions.items():
        c_content.append(f"static void parse_json_to_{struct_name.lower()}(json_t* json_obj, {struct_name}* out_struct) {{")
        c_content.append(f"    if (!json_is_object(json_obj)) {{ fprintf(stderr, \"Error: Expected JSON object for parsing into {struct_name}, but got %s.\\n\", json_typeof_str(json_typeof(json_obj))); exit(EXIT_FAILURE); }}")
        c_content.append(f"    if (!out_struct) {{ fprintf(stderr, \"Error: NULL out_struct provided to parse_json_to_{struct_name.lower()}\\n\"); exit(EXIT_FAILURE); }}")

        for field_name, field_info in fields.items():
            original_json_key = field_info['json_key']
            c_content.append(f"    // Parsing field: {original_json_key} -> {field_name}")
            c_content.append(f"    json_t* j_{field_name} = json_object_get(json_obj, \"{original_json_key}\");")
            c_content.append(f"    if (j_{field_name} && !json_is_null(j_{field_name})) {{")

            if field_info['is_array']:
                arr_elem_c_type = field_info['array_element_c_type']
                arr_elem_is_ptr = field_info['array_element_is_pointer']
                nested_s_name = field_info['nested_struct_def_name']
                c_content.append(f"        if (json_is_array(j_{field_name})) {{")
                c_content.append(f"            size_t json_array_size_val = json_array_size(j_{field_name});")
                c_content.append(f"            out_struct->{field_name} = NULL;") # Initialize for stb_ds
                # REMOVED: out_struct->{field_name}_count = 0;
                c_content.append(f"            for (size_t i = 0; i < json_array_size_val; ++i) {{")
                c_content.append(f"                json_t* item_json = json_array_get(j_{field_name}, i);")
                c_content.append(f"                if (!item_json) {{ fprintf(stderr, \"Error: Failed to get item %zu from array '{original_json_key}' in {struct_name}.\\n\", i); exit(EXIT_FAILURE); }}")

                if arr_elem_c_type == "char*" and arr_elem_is_ptr:
                    c_content.append(f"                if (json_is_string(item_json)) {{")
                    c_content.append(f"                    const char* str_val = json_string_value(item_json);")
                    c_content.append(f"                    char* new_str = str_val ? strdup(str_val) : NULL;")
                    c_content.append(f"                    if (str_val && !new_str) {{ fprintf(stderr, \"Error: strdup failed for string in array '{original_json_key}' in {struct_name}.\\n\"); exit(EXIT_FAILURE); }}")
                    c_content.append(f"                    arrput(out_struct->{field_name}, new_str);")
                    # REMOVED: out_struct->{field_name}_count++;
                    c_content.append(f"                }} else if (!json_is_null(item_json)) {{ fprintf(stderr, \"Error: Expected string in array '{original_json_key}', got %s in {struct_name}.\\n\", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }}")
                elif arr_elem_c_type == "int":
                    c_content.append(f"                if (json_is_integer(item_json)) {{ arrput(out_struct->{field_name}, (int)json_integer_value(item_json)); }}") # REMOVED count increment
                    c_content.append(f"                else if (json_is_boolean(item_json)) {{ arrput(out_struct->{field_name}, json_is_true(item_json) ? 1 : 0); }}") # REMOVED count increment
                    c_content.append(f"                else if (!json_is_null(item_json)) {{ fprintf(stderr, \"Error: Expected int/bool in array '{original_json_key}', got %s in {struct_name}.\\n\", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }}")
                elif arr_elem_c_type == "double":
                    c_content.append(f"                if (json_is_number(item_json)) {{ arrput(out_struct->{field_name}, json_number_value(item_json)); }}") # REMOVED count increment
                    c_content.append(f"                else if (!json_is_null(item_json)) {{ fprintf(stderr, \"Error: Expected number in array '{original_json_key}', got %s in {struct_name}.\\n\", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }}")
                elif arr_elem_c_type.startswith("struct ") and not arr_elem_is_ptr:
                    c_content.append(f"                if (json_is_object(item_json)) {{")
                    c_content.append(f"                    {arr_elem_c_type} temp_item;")
                    c_content.append(f"                    memset(&temp_item, 0, sizeof({arr_elem_c_type}));")
                    c_content.append(f"                    parse_json_to_{nested_s_name.lower()}(item_json, &temp_item);")
                    c_content.append(f"                    arrput(out_struct->{field_name}, temp_item);")
                    # REMOVED: out_struct->{field_name}_count++;
                    c_content.append(f"                }} else if (!json_is_null(item_json)) {{ fprintf(stderr, \"Error: Expected object in array '{original_json_key}', got %s in {struct_name}.\\n\", json_typeof_str(json_typeof(item_json))); exit(EXIT_FAILURE); }}")
                c_content.append(f"            }}")
                c_content.append(f"        }} else {{ fprintf(stderr, \"Error: Expected array for '{original_json_key}', got %s in {struct_name}.\\n\", json_typeof_str(json_typeof(j_{field_name}))); exit(EXIT_FAILURE); }}")
            # ... (rest of the field parsing logic for non-arrays remains the same) ...
            elif field_info['field_c_type'] == "char*":
                c_content.append(f"        if (json_is_string(j_{field_name})) {{")
                c_content.append(f"            const char* str_val = json_string_value(j_{field_name});")
                c_content.append(f"            if (str_val) {{")
                c_content.append(f"                out_struct->{field_name} = strdup(str_val);")
                c_content.append(f"                if (!out_struct->{field_name}) {{ fprintf(stderr, \"Error: strdup failed for '{original_json_key}' in {struct_name}.\\n\"); exit(EXIT_FAILURE); }}")
                c_content.append(f"            }}")
                c_content.append(f"        }} else {{ fprintf(stderr, \"Error: Expected string for '{original_json_key}', got %s in {struct_name}.\\n\", json_typeof_str(json_typeof(j_{field_name}))); exit(EXIT_FAILURE); }}")
            elif field_info['field_c_type'] == "int":
                c_content.append(f"        if (json_is_integer(j_{field_name})) {{ out_struct->{field_name} = (int)json_integer_value(j_{field_name}); }}")
                c_content.append(f"        else if (json_is_boolean(j_{field_name})) {{ out_struct->{field_name} = json_is_true(j_{field_name}) ? 1 : 0; }}")
                c_content.append(f"        else {{ fprintf(stderr, \"Error: Expected int/bool for '{original_json_key}', got %s in {struct_name}.\\n\", json_typeof_str(json_typeof(j_{field_name}))); exit(EXIT_FAILURE); }}")
            elif field_info['field_c_type'] == "double":
                c_content.append(f"        if (json_is_number(j_{field_name})) {{ out_struct->{field_name} = json_number_value(j_{field_name}); }}")
                c_content.append(f"        else {{ fprintf(stderr, \"Error: Expected number for '{original_json_key}', got %s in {struct_name}.\\n\", json_typeof_str(json_typeof(j_{field_name}))); exit(EXIT_FAILURE); }}")
            elif field_info['is_single_struct_pointer']:
                nested_s_name = field_info['nested_struct_def_name']
                c_content.append(f"        if (json_is_object(j_{field_name})) {{")
                c_content.append(f"            out_struct->{field_name} = ({field_info['field_c_type']})calloc(1, sizeof({field_info['nested_struct_def_name']}));")
                c_content.append(f"            if (!out_struct->{field_name}) {{ fprintf(stderr, \"Error: calloc failed for '{original_json_key}' in {struct_name}.\\n\"); exit(EXIT_FAILURE); }}")
                c_content.append(f"            parse_json_to_{nested_s_name.lower()}(j_{field_name}, out_struct->{field_name});")
                c_content.append(f"        }} else {{ fprintf(stderr, \"Error: Expected object for '{original_json_key}', got %s in {struct_name}.\\n\", json_typeof_str(json_typeof(j_{field_name}))); exit(EXIT_FAILURE); }}")
            c_content.append(f"    }} else if (!j_{field_name}) {{ /* Key not found, field remains default */ }}")
            c_content.append(f"    else {{ /* Key is null, field remains default */ }}")
        c_content.append("}\n")

    # Main Parsing Functions (same as previous "exit on error" version)
    c_content.append("\n// --- Main Parsing Function (exit on error) ---")
    c_content.append(f"{root_struct_name}* parse_{root_struct_name.lower()}_from_string(const char* json_string) {{")
    c_content.append("    if (!json_string) { fprintf(stderr, \"Error: NULL JSON string provided.\\n\"); exit(EXIT_FAILURE); }")
    c_content.append("    json_error_t error;")
    c_content.append("    json_t* root_json = json_loads(json_string, 0, &error);")
    c_content.append("    if (!root_json) { fprintf(stderr, \"JSON parsing error at string: %s (line %d, column %d)\\n\", error.text, error.line, error.column); exit(EXIT_FAILURE); }")
    c_content.append(f"    if (!json_is_object(root_json)) {{ fprintf(stderr, \"Error: Root JSON is not an object, got %s.\\n\", json_typeof_str(json_typeof(root_json))); json_decref(root_json); exit(EXIT_FAILURE); }}")
    c_content.append(f"    {root_struct_name}* result = ({root_struct_name}*)calloc(1, sizeof({root_struct_name}));")
    c_content.append("    if (!result) { fprintf(stderr, \"Memory allocation failed for root struct\\n\"); json_decref(root_json); exit(EXIT_FAILURE); }")
    c_content.append(f"    parse_json_to_{root_struct_name.lower()}(root_json, result);")
    c_content.append("    json_decref(root_json);")
    c_content.append("    return result;")
    c_content.append("}\n")

    c_content.append(f"{root_struct_name}* parse_{root_struct_name.lower()}_from_file(const char* filename) {{")
    c_content.append("    if (!filename) { fprintf(stderr, \"Error: NULL filename provided.\\n\"); exit(EXIT_FAILURE); }")
    c_content.append("    FILE* fp = fopen(filename, \"rb\");")
    c_content.append("    if (!fp) { perror(\"Error opening file\"); fprintf(stderr, \"Filename: %s\\n\", filename); exit(EXIT_FAILURE); }")
    c_content.append("    fseek(fp, 0, SEEK_END); long length = ftell(fp); fseek(fp, 0, SEEK_SET);")
    c_content.append("    if (length < 0) { fprintf(stderr, \"Error: Could not determine size of file %s\\n\", filename); fclose(fp); exit(EXIT_FAILURE); }")
    c_content.append("    char* buffer = (char*)malloc(length + 1);")
    c_content.append("    if (!buffer) { fprintf(stderr, \"Memory allocation failed for file buffer (%ld bytes) for %s\\n\", length + 1, filename); fclose(fp); exit(EXIT_FAILURE); }")
    c_content.append("    size_t read_len = fread(buffer, 1, length, fp);")
    c_content.append("    if (read_len != (size_t)length) { fprintf(stderr, \"Error: Failed to read entire file %s (read %zu of %ld bytes)\\n\", filename, read_len, length); free(buffer); fclose(fp); exit(EXIT_FAILURE); }")
    c_content.append("    fclose(fp); buffer[length] = '\\0';")
    c_content.append(f"    {root_struct_name}* result = parse_{root_struct_name.lower()}_from_string(buffer);")
    c_content.append("    free(buffer);")
    c_content.append("    return result;")
    c_content.append("}\n")

    return "\n".join(c_content)

# --- Main Script Logic (same as previous version) ---
def main():
    parser = argparse.ArgumentParser(description="Generate C header and source files from a JSON file.")
    parser.add_argument("json_file", help="Path to the input JSON file.")
    parser.add_argument("-o", "--output", help="Base name for output .h and .c files. Defaults to JSON filename.")
    parser.add_argument("-r", "--root_struct_name", help="Name for the root C struct. Defaults to 'RootData' or derived.", default="RootData")
    args = parser.parse_args()

    base_name = args.output if args.output else os.path.splitext(os.path.basename(args.json_file))[0]
    base_name = sanitize_c_identifier(base_name)
    
    root_struct_name = to_camel_case(sanitize_c_identifier(args.root_struct_name if args.root_struct_name != "RootData" or base_name == "rootdata" else base_name))
    if not root_struct_name: root_struct_name = "RootData"

    try:
        with open(args.json_file, 'r') as f: data = json.load(f)
    except Exception as e:
        print(f"Error loading JSON file '{args.json_file}': {e}"); return

    if not isinstance(data, dict): print("Error: Root of JSON must be an object."); return

    all_struct_definitions = {}
    structs_to_generate_queue = {root_struct_name: data}
    processed_struct_names = set()
    ordered_struct_names = []

    while structs_to_generate_queue:
        current_struct_name, current_json_data = structs_to_generate_queue.popitem()
        if current_struct_name in all_struct_definitions: continue
        
        ordered_struct_names.append(current_struct_name)
        all_struct_definitions[current_struct_name] = {}
        processed_struct_names.add(current_struct_name)

        if not isinstance(current_json_data, dict):
            print(f"Warning: Expected object for struct {current_struct_name}, got {type(current_json_data)}. Skipping fields.")
            continue

        for key, value in current_json_data.items():
            c_field_name = sanitize_c_identifier(key)
            field_details = get_c_type_info(value, key, "", structs_to_generate_queue, processed_struct_names)
            all_struct_definitions[current_struct_name][c_field_name] = { 'json_key': key, **field_details }
    
    temp_defs = {}
    if root_struct_name in ordered_struct_names: ordered_struct_names.remove(root_struct_name)
    ordered_struct_names.insert(0, root_struct_name)
    for s_name in ordered_struct_names:
        if s_name in all_struct_definitions: temp_defs[s_name] = all_struct_definitions[s_name]
    all_struct_definitions = temp_defs

    header_code = generate_header_file(base_name, root_struct_name, all_struct_definitions)
    c_code = generate_c_file(base_name, root_struct_name, all_struct_definitions)

    with open(f"{base_name}.h", 'w') as f: f.write(header_code)
    print(f"Generated {base_name}.h")
    with open(f"{base_name}.c", 'w') as f: f.write(c_code)
    print(f"Generated {base_name}.c")

    print("\nCompilation hint (ensure jansson is installed):")
    print(f"  gcc -c {base_name}.c -o {base_name}.o -I/path/to/jansson/include -Wall -Wextra -g")
    print(f"  gcc your_main.c {base_name}.o -o my_program -L/path/to/jansson/lib -ljansson -Wall -Wextra -g")
    print("  (stb_ds.h is included with STB_DS_IMPLEMENTATION in the generated .c file)")
    print("  Remember to use arrlen(my_struct->array_field) to get array lengths.")

if __name__ == "__main__":
    main()