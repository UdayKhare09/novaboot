#!/usr/bin/env python3
import sys
import re

def main():
    if len(sys.argv) < 3:
        print("Usage: odb_bridge.py <input_header> <output_header>")
        sys.exit(1)

    input_path = sys.argv[1]
    output_path = sys.argv[2]

    with open(input_path, 'r') as f:
        content = f.read()

    # Prepend ODB core header
    processed = "#include <odb/core.hxx>\n" + content

    # 1. Strip any non-database standard C++26 attributes first to prevent ODB compiler errors
    # and simplify struct entity regex matching
    processed = re.sub(
        r'\[\[\s*=\s*(?!(?:novaboot\s*::\s*)?data\s*::).*?\s*\]\]',
        r'',
        processed
    )

    # 2. Regex replacements for C++26 standard annotations to ODB C++17 pragmas
    # struct [[=data::entity{"table_name"}]] ClassName -> #pragma db object table("table_name") \n struct ClassName
    processed = re.sub(
        r'struct\s+\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*entity\s*\{\s*"([^"]*)"\s*\}\s*\]\]\s*(\w+)',
        r'#pragma db object table("\1")\nstruct \2',
        processed
    )
    # struct [[=data::entity{}]] ClassName -> #pragma db object \n struct ClassName
    processed = re.sub(
        r'struct\s+\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*entity\s*(?:\{\s*\})?\s*\]\]\s*(\w+)',
        r'#pragma db object\nstruct \1',
        processed
    )
    # [[=data::id{}]] or [[=data::id]] -> #pragma db id
    processed = re.sub(
        r'\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*id\s*(?:\{\s*\})?\s*\]\]',
        r'\n#pragma db id\n',
        processed
    )
    # [[=data::column{"column_name"}]] -> #pragma db column("column_name")
    processed = re.sub(
        r'\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*column\s*\{\s*"([^"]*)"\s*\}\s*\]\]',
        r'\n#pragma db column("\1")\n',
        processed
    )
    # [[=data::transient{}]] or [[=data::transient]] -> #pragma db transient
    processed = re.sub(
        r'\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*transient\s*(?:\{\s*\})?\s*\]\]',
        r'\n#pragma db transient\n',
        processed
    )
    # [[=data::version{}]] or [[=data::version]] -> #pragma db version
    processed = re.sub(
        r'\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*version\s*(?:\{\s*\})?\s*\]\]',
        r'\n#pragma db version\n',
        processed
    )
    # struct/class [[=data::value{}]] -> #pragma db value
    processed = re.sub(
        r'(struct|class)\s+\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*value\s*(?:\{\s*\})?\s*\]\]\s*(\w+)',
        r'#pragma db value\n\1 \2',
        processed
    )

    # [[=data::unique]] Type name; -> #pragma db index("name_unique") unique members(name) \n Type name;
    processed = re.sub(
        r'\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*unique\s*(?:\{\s*\})?\s*\]\]\s*([^;]+)\s+(\w+)\s*;',
        r'#pragma db index("\2_unique") unique members(\2)\n\1 \2;',
        processed
    )
    # [[=data::not_null]] Type name; -> #pragma db member(name) not_null \n Type name;
    processed = re.sub(
        r'\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*not_null\s*(?:\{\s*\})?\s*\]\]\s*([^;]+)\s+(\w+)\s*;',
        r'#pragma db member(\2) not_null\n\1 \2;',
        processed
    )
    # [[=data::nullable]] Type name; -> #pragma db member(name) null \n Type name;
    processed = re.sub(
        r'\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*nullable\s*(?:\{\s*\})?\s*\]\]\s*([^;]+)\s+(\w+)\s*;',
        r'#pragma db member(\2) null\n\1 \2;',
        processed
    )
    # [[=data::relation{"options"}]] Type name; -> #pragma db member(name) relation("options") \n Type name;
    processed = re.sub(
        r'\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*relation\s*\{\s*"([^"]*)"\s*\}\s*\]\]\s*([^;]+)\s+(\w+)\s*;',
        r'#pragma db member(\3) relation(\1)\n\2 \3;',
        processed
    )
    # [[=data::relation]] Type name; -> #pragma db member(name) relation \n Type name;
    processed = re.sub(
        r'\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*relation\s*(?:\{\s*\})?\s*\]\]\s*([^;]+)\s+(\w+)\s*;',
        r'#pragma db member(\2) relation\n\1 \2;',
        processed
    )
    # [[=data::lazy]] Type name; -> #pragma db member(name) lazy \n Type name;
    processed = re.sub(
        r'\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*lazy\s*(?:\{\s*\})?\s*\]\]\s*([^;]+)\s+(\w+)\s*;',
        r'#pragma db member(\2) lazy\n\1 \2;',
        processed
    )
    # [[=data::index{"name"}]] Type name; -> #pragma db index("name") members(name) \n Type name;
    processed = re.sub(
        r'\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*index\s*\{\s*"([^"]*)"\s*\}\s*\]\]\s*([^;]+)\s+(\w+)\s*;',
        r'#pragma db index("\1") members(\3)\n\2 \3;',
        processed
    )
    # [[=data::index]] Type name; -> #pragma db index members(name) \n Type name;
    processed = re.sub(
        r'\[\[\s*=\s*(?:novaboot\s*::\s*)*data\s*::\s*index\s*(?:\{\s*\})?\s*\]\]\s*([^;]+)\s+(\w+)\s*;',
        r'#pragma db index members(\2)\n\1 \2;',
        processed
    )
    # Strip any remaining C++26 standard attributes to prevent C++17 ODB compiler errors
    processed = re.sub(
        r'\[\[\s*=.*?\s*\]\]',
        r'',
        processed
    )

    with open(output_path, 'w') as f:
        f.write(processed)

if __name__ == '__main__':
    main()
