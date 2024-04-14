#ifndef COMPILER_H_
#define COMPILER_H_

#include "parser.h"

String_View lasm_load_file(Arena *arena, const char *file_path);
String_View lasm_cut_comments_from_src(String_View *sv);

void lasm_cut_comments_from_line(String_View *line);
void lasm_save_program_to_file(CPU *c, const char *file_path);
void lasm_translate_source(Arena *arena, String_View src, CPU *c, Program_Jumps *PJ, Hash_Table *ht, Const_Table *ct,
                           int db_lex, int db_lnz, int db_ht, int db_line, int db_lex_txt, int db_bc);

#endif // COMPILER_H_