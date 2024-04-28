#include "../include/parser.h"

void ll_append(Arena *arena, Label_List *ll, Label label)
{
    if (ll->capacity == 0) {
        ll->capacity = INIT_CAPACITY;
        ll->labels = arena_alloc(arena, sizeof(ll->labels[0]) * ll->capacity);
    }

    if (ll->count + 1 > ll->capacity) {
        size_t old_size = ll->capacity * sizeof(*ll->labels);
        ll->capacity *= 2;
        ll->labels = arena_realloc(arena, ll->labels, old_size, ll->capacity * sizeof(*ll->labels));
    }

    ll->labels[ll->count++] = label;
}

Label ll_search_label(Label_List *ll, String_View name)
{
    for (size_t i = 0; i < ll->count; ++i) {
        Label label = ll->labels[i];
        if (sv_cmp(label.name, name)) {
            return label;
        }
    }
    return (Label) {
        .addr = -1
    };
}

void ll_print(Label_List *ll)
{
    printf("\n--------- Label List -----------\n\n");
    for (size_t i = 0; i < ll->count; ++i) {
        Label label = ll->labels[i];
        printf("name: {"SV_Fmt"}; addr: {%lu}\n", SV_Args(label.name), label.addr);
    }
    printf("\n--------------------------------\n\n");
}

Token parse_val(Lexer *lex, Const_Table *ct)
{
    Eval eval = {0};
    if (token_get(lex, 0, SKIP_FALSE).type == TYPE_AMPERSAND &&
        (size_t)lex->tp + 2 >= lex->count) {
        token_next(lex); // skip &
        Token tk = token_next(lex);
        if (tk.type != TYPE_TEXT) {
            fprintf(stderr, "Error: expected token with type `text` after `&`\n");
            exit(1);
        }
        return parse_constant_expr(tk, ct);
    }
    parse_arefmetic_expr(&eval, lex, ct);
    evaluate(&eval);
    Token tk = eval.root->token; 
    eval_clean(eval.root, &eval.count);
    return tk;
}

void objb_push(Arena *arena, Object_Block *objb, Object obj)
{
    da_append(arena, objb, obj);
}

void block_chain_push(Arena *arena, Block_Chain *block_chain, Object_Block objb)
{
    da_append(arena, block_chain, objb);
}

Object translate_val_expr_to_obj(Lexer *lex, Const_Table *ct)
{
    Token tk = parse_val(lex, ct);
    if (tk.type == TYPE_VALUE) {
        switch (tk.val.type) {
            case VAL_INT:   return OBJ_INT(tk.val.i64); 
            case VAL_FLOAT: return OBJ_FLOAT(tk.val.f64);
            default:
                fprintf(stderr, "Error: unknown type `%u` in translation", tk.val.type);
                exit(1);
        }
    } else {
        fprintf(stderr, "Error: token not a value; type `%u`\n", tk.type);
        exit(1);
    }
}

Object translate_reg_to_obj(Token tk)
{
    if (tk.type == TYPE_TEXT) {
        int reg = parse_register(tk.txt);
        if (reg == -1) {
            fprintf(stderr, "Error: `%d` not a register", reg);
            exit(1);
        }
        return OBJ_REG(reg);
    } else {
        fprintf(stderr, "Error: for translating register needs `TYPE_TEXT`\n");
        exit(1);
    }
}

void parse_kind_reg_reg(Arena *arena, Inst_Addr *inst_pointer, Lexer *lex, Object_Block *objb)
{
    for (Token tk = lex->items[lex->tp + 1]; 
        tk.type != TYPE_NONE;
        tk = token_next(lex)) {

        if (tk.type == TYPE_COMMA) continue;
        else if (tk.type == TYPE_TEXT) {
            Object obj = translate_reg_to_obj(tk);
            objb_push(arena, objb, obj);
        } else {
            // TODO: better errors
            fprintf(stderr, "Error: in `parse_kind_reg_reg` cannot parse register\n");
            exit(1);
        }
    }
    *inst_pointer += 3;
}

void parse_kind_reg_val(Arena *arena, Inst_Addr *inst_pointer, Lexer *lex, Object_Block *objb, Const_Table *ct)
{
    for (Token tk = lex->items[lex->tp + 1]; 
        tk.type != TYPE_NONE;
        tk = token_next(lex)) {
        
        Object obj = {0};
        if (tk.type == TYPE_VALUE ||
            tk.type == TYPE_AMPERSAND ||
            tk.type == TYPE_OPEN_BRACKET) {

            token_back(lex, 1);
            obj = translate_val_expr_to_obj(lex, ct);

        } else if (tk.type == TYPE_TEXT) {
            obj = translate_reg_to_obj(tk);

        } else if (tk.type == TYPE_COMMA) {
            continue;

        } else if (tk.type == TYPE_DOLLAR) {
            tk = token_next(lex);
            if (tk.type != TYPE_VALUE)
                fprintf(stderr, "Error: after `$` in `mov` not a value, type `%u`", tk.type);
            int64_t frame_shift = tk.val.i64 + STACK_FRAME_SIZE;
            obj = OBJ_INT(frame_shift);

        } else {
            // TODO: better erros
            assert(0 && "TODO: in `parse_kind_reg_val` implement condition");
        }
        objb_push(arena, objb, obj);
    }
    *inst_pointer += 3;
}

void parse_kind_reg(Arena *arena, Inst_Addr *inst_pointer, Lexer *lex, Object_Block *objb)
{
    Token tk = token_next(lex);
    Object obj = translate_reg_to_obj(tk);
    objb_push(arena, objb, obj);
    *inst_pointer += 2;
}

void parse_kind_val(Arena *arena,
                    Inst inst, 
                    Inst_Addr *inst_pointer,
                    Lexer *lex, 
                    Object_Block *objb, 
                    Program_Jumps *PJ,
                    Const_Table *ct,
                    size_t line_num)
{
    switch (inst) {
        case INST_PUSH_V: {
            Token tk = token_get(lex, 0, SKIP_FALSE);
            if (tk.type == TYPE_OPEN_BRACKET ||
                tk.type == TYPE_AMPERSAND||
                tk.type == TYPE_VALUE) {

                Object obj = translate_val_expr_to_obj(lex, ct);
                objb_push(arena, objb, obj);

            } else {
                fprintf(stderr, "Error: unknown type `%u` in `parse_kind_val`\n", tk.type);
                exit(1);
            }
        }
        break;

        case INST_JZ:
        case INST_JNZ:
        case INST_JMP:
        case INST_CALL:
            Object addr_obj;
            Token tk = token_next(lex);
            if (tk.type == TYPE_VALUE) {
                if (tk.val.type == VAL_INT && tk.val.i64 >= 0) {
                    addr_obj = OBJ_UINT(tk.val.i64);
                    objb_push(arena, objb, addr_obj);
                } else {
                    fprintf(stderr, "Error: program jumps have to be integer and > 0\n");
                    exit(1);
                }
            } else if (tk.type == TYPE_TEXT) {
                Label label = ll_search_label(&PJ->current, tk.txt);
                if (label.addr == (uint64_t)(-1)) {
                    Label dlabel = {
                        .name = tk.txt,
                        .addr = line_num
                    };
                    ll_append(arena, &PJ->deferred, dlabel);
                    addr_obj = OBJ_UINT(dlabel.addr);
                    objb_push(arena, objb, addr_obj);

                } else {
                    addr_obj = OBJ_UINT(label.addr);
                    objb_push(arena, objb, addr_obj);
                }

            } else {
                fprintf(stderr, "Error: inst `%s` expects label or addr\n", inst_as_cstr(inst));
                exit(1);
            }
            break;

        default: {
            fprintf(stderr, "Error: inst `%s` unsupported program jumps or stack operations\n", inst_as_cstr(inst));
            exit(1);
        }
    }
    *inst_pointer += 2;
}

Inst parse_inst(Lexer *lex, Hash_Table *ht)
{
    Token tk = token_next(lex);
    int inst = translate_inst(tk.txt, ht);
    if (inst == -1) {
        fprintf(stderr, "Error: unknown inst\n");
        exit(1);
    } else {
        return (Inst)(inst);
    }
}

int try_register(String_View sv)
{
    if (sv.data[0] == 'r' || sv.data[0] == 'f') {
        if (isdigit(sv.data[1])) {
            return 1;
        } else {
            return 0;
        }
    } else if (sv_cmp(sv, sv_from_cstr("acc"))) {
        return 1;
    } else if (sv_cmp(sv, sv_from_cstr("accf"))) {
        return 1;
    } else {
        return 0;
    }
}

// TODO: when error occur show line with uncorrect syntax
Inst convert_to_cpu_inst(Inst inst, Inst_Kind *inst_kind, Lexer *lex)
{
    Inst new_inst;
    Inst_Kind kind;

    Token tk = token_get(lex, 0, SKIP_FALSE);
    if (tk.type == TYPE_NONE) {
        kind = KIND_NONE;
    } else {
        tk = token_get(lex, 1, SKIP_FALSE);
        if (tk.type == TYPE_COMMA) {
            tk = token_get(lex, 2, SKIP_FALSE);
            if (tk.type == TYPE_VALUE || tk.type == TYPE_OPEN_BRACKET || tk.type == TYPE_DOLLAR || tk.type == TYPE_AMPERSAND) {
                kind = KIND_REG_VAL;
            } else {
                if (try_register(tk.txt)) kind = KIND_REG_REG;
                else kind = KIND_REG_VAL;
            }
        } else {
            tk = token_get(lex, 0, SKIP_FALSE);
            if ((tk.type == TYPE_OPEN_BRACKET || tk.type == TYPE_VALUE || tk.type == TYPE_AMPERSAND)) {
                kind = KIND_VAL;
            } else {
                if (try_register(tk.txt)) 
                    kind = KIND_REG;
                else 
                    kind = KIND_VAL;
            }
        }
    }

    *inst_kind = kind;

    switch (kind) {
        case KIND_REG_REG: {
            switch (inst) {
                case INST_ADD:
                    new_inst = INST_ADD_RR;
                    break;

                case INST_SUB:
                    new_inst = INST_SUB_RR;
                    break;

                case INST_DIV:
                    new_inst = INST_DIV_RR;
                    break;

                case INST_MUL:
                    new_inst = INST_MUL_RR;
                    break;

                case INST_MOV:
                    new_inst = INST_MOV_RR;
                    break;

                case INST_CMP:
                    new_inst = inst;
                    break;

                case INST_RET:
                    new_inst = INST_RET_RR;
                    break; 

                default:
                    fprintf(stderr, "inst `%s` hasn't got kind `REG_REG`\n", inst_as_cstr(inst));
                    exit(1);
            }
        }
        break;

        case KIND_REG_VAL: {
            switch (inst) {
                case INST_ADD:
                    new_inst = INST_ADD_RV;
                    break;

                case INST_SUB:
                    new_inst = INST_SUB_RV;
                    break;

                case INST_DIV:
                    new_inst = INST_DIV_RV;
                    break;

                case INST_MUL:
                    new_inst = INST_MUL_RV;
                    break;
                    
                case INST_MOV:
                    tk = token_get(lex, 2, SKIP_FALSE);
                    if (tk.type == TYPE_DOLLAR) {
                        new_inst = INST_MOVS;
                    } else {
                        new_inst = INST_MOV_RV;
                    }
                    break;

                case INST_CMP:
                    new_inst = inst;
                    break;

                case INST_RET:
                    new_inst = INST_RET_RV;
                    break;

                default:
                    fprintf(stderr, "inst `%s` hasn't got kind `REG_VAL`\n", inst_as_cstr(inst));
                    exit(1);
            }
        }
        break;

        case KIND_REG: {
            switch (inst) {
                case INST_PUSH:
                    new_inst = INST_PUSH_R;
                    break;

                case INST_POP:
                    new_inst = INST_POP_R;
                    break;

                case INST_DBR:
                    new_inst = inst;
                    break;

                default:
                    fprintf(stderr, "inst `%s` hasn't got kind `REG`\n", inst_as_cstr(inst));
                    exit(1);
            }
        }
        break;

        case KIND_VAL: {
            switch (inst) {
                case INST_PUSH:
                    new_inst = INST_PUSH_V;
                    break;

                case INST_JZ:
                case INST_JMP:
                case INST_JNZ:
                case INST_CALL:
                    new_inst = inst;
                    break;

                default:
                    fprintf(stderr, "inst `%s` hasn't got kind `VAL`\n", inst_as_cstr(inst));
                    exit(1);
            }
        }
        break;

        case KIND_NONE: {
            switch (inst) {
                case INST_POP:
                    new_inst = INST_POP_N;
                    break;

                case INST_RET:
                    new_inst = INST_RET_N;
                    break;

                case INST_HLT:
                case INST_VLAD:
                    new_inst = inst;
                    break;

                default:
                    fprintf(stderr, "inst `%s` hasn't got kind `none`\n", inst_as_cstr(inst));
                    exit(1);
            }
        }
        break;

        default: {
            fprintf(stderr, "Error: unknown kind `%u`\n", kind);
            break;
        }
    }
    return new_inst;
}

// TODO: better errors
Const_Statement parse_line_constant(Lexer *lex)
{
    Const_Statement cnst = {0};
    
    Token cnst_name = token_next(lex);
    if (cnst_name.type != TYPE_TEXT) {
        fprintf(stderr, "Error: uncorrect name for constant. type: `%u`\n", cnst_name.type);
        exit(1);
    }

    cnst.name = cnst_name.txt;

    Token open_curly = token_next(lex);
    if (open_curly.type != TYPE_OPEN_CURLY) {
        fprintf(stderr, "Error: after name expectes `{`\n");
        exit(1);
    }

    Token cnst_type = token_next(lex);
    if (cnst_type.type != TYPE_TEXT) {
        fprintf(stderr, "Error: expectes text for defining type\n");
        exit(1);
    }

    if (sv_cmp(cnst_type.txt, sv_from_cstr("i64"))) {
        cnst.type = CONST_TYPE_INT;

    } else if (sv_cmp(cnst_type.txt, sv_from_cstr("u64"))) {
        cnst.type = CONST_TYPE_UINT;

    } else if (sv_cmp(cnst_type.txt, sv_from_cstr("f64"))) {
        cnst.type = CONST_TYPE_FLOAT;

    } else {
        fprintf(stderr, "Error: unknown type `"SV_Fmt"` for constant\n", SV_Args(cnst_type.txt));
        exit(1);
    }

    Token semicolon = token_next(lex);
    if (semicolon.type != TYPE_COLON) {
        fprintf(stderr, "Error: expected `:` after type\n");
        exit(1);
    }

    // TODO: check that value and type are the same
    Token cnst_value = token_next(lex);
    switch (cnst.type) {
        case CONST_TYPE_INT:
            if (cnst_value.val.type != VAL_INT) {
                fprintf(stderr, "Error: after type `i64` expectes integer\n");
                exit(1);
            }
            cnst.as_i64 = cnst_value.val.i64;
            break;

        case CONST_TYPE_UINT:
            if (cnst_value.val.type != VAL_INT) {
                fprintf(stderr, "Error: after type `u64` expectes unsigned integer\n");
                exit(1);
            }
            if (cnst_value.val.i64 < 0 ) {
                fprintf(stderr, "Error: unsigned value cannot be less than 0\n");
                exit(1);    
            }
            cnst.as_u64 = cnst_value.val.i64;
            break;

        case CONST_TYPE_FLOAT:
            if (cnst_value.val.type != VAL_FLOAT) {
                fprintf(stderr, "Error: after type `f64` expectes float\n");
                exit(1);
            }
            cnst.as_f64 = cnst_value.val.f64;
            break;

        default:
            fprintf(stderr, "Error: unknown type in `parse_line_constant`\n");
            exit(1);
    }

    return cnst;
}

Object_Block parse_line_inst(Arena *arena, Line line, Hash_Table *ht, Program_Jumps *PJ, Const_Table *ct,
                             size_t inst_counter, size_t *inst_pointer, 
                             int db_line, size_t line_num)
{
    Inst_Kind kind;
    Object_Block objb = {0};
    Lexer sublex = line.item;
    Inst src_inst = parse_inst(&sublex, ht);
    Inst cpu_inst = convert_to_cpu_inst(src_inst, &kind, &sublex);
    objb_push(arena, &objb, OBJ_INST(cpu_inst));
    
    if (db_line) {
        printf("line: %zu; ", line_num + 1);
        printf("inst: %s; kind: %u\n", inst_as_cstr(cpu_inst), kind);
    }

    switch (kind) {
        case KIND_REG:     parse_kind_reg(arena, inst_pointer, &sublex, &objb);                                 break;
        case KIND_VAL:     parse_kind_val(arena, cpu_inst, inst_pointer, &sublex, &objb, PJ, ct, inst_counter); break;
        case KIND_NONE:    *inst_pointer += 1;                                                                  break;
        case KIND_REG_REG: parse_kind_reg_reg(arena, inst_pointer, &sublex, &objb);                             break;
        case KIND_REG_VAL: parse_kind_reg_val(arena, inst_pointer, &sublex, &objb, ct);                         break;
        default: {
            fprintf(stderr, "Error: unknown kind `%u`\n", kind);
            exit(1);
        }
    }

    return objb;
}

// TODO: rework line debug
void parse_line_label(Arena *arena, Token tk, Program_Jumps *PJ, size_t inst_pointer, size_t line_num)
{
    if (tk.type != TYPE_TEXT) {
        fprintf(stderr, "Error: in line [%zu] expected label\n", line_num + 1);
        exit(1);
    }
    Label label = { .name = tk.txt, .addr = inst_pointer};
    ll_append(arena, &PJ->current, label);
}

void block_chain_debug(Block_Chain *bc)
{
    printf("\n---------------------------------------------\n\n");
    for (size_t i = 0; i < bc->count; ++i) {
        Object_Block objb = bc->items[i];
        printf("object block: %zu\n", i);
        for (size_t j = 0; j < objb.count; ++j) {
            Object o = objb.items[j];
            printf("inst: %u, reg: %u, i64: %li, u64: %lu, f64: %lf\n",
                    o.inst, o.reg, o.i64, o.u64, o.f64);
        }
        printf("\n");
    }
    printf("\n---------------------------------------------\n\n");
}

Block_Chain parse_linizer(Arena *arena, Linizer *lnz, Program_Jumps *PJ, Hash_Table *ht, Const_Table *ct, int line_debug, int bc_debug)
{
    size_t entry_ip = 0;
    size_t inst_counter = 0;
    Inst_Addr inst_pointer = 0;
    Block_Chain block_chain = {0};
    for (size_t i = 0; i < lnz->count; ++i) {
        Line line = lnz->items[i];
        if (line.type == LINE_INST) {
            Object_Block objb = parse_line_inst(arena, line, ht, PJ, ct,
                                                inst_counter, &inst_pointer, line_debug, i);
            block_chain_push(arena, &block_chain, objb);
            inst_counter += 1;

        } else if (line.type == LINE_LABEL) {
            Token tk = line.item.items[0];
            parse_line_label(arena, tk, PJ, inst_pointer, i);

        } else if (line.type == LINE_ENTRY_LABLE) {
            entry_ip = inst_pointer;
            Token tk = line.item.items[0];
            parse_line_label(arena, tk, PJ, inst_pointer, i);

        } else if (line.type == LINE_CONSTANT) {
            Const_Statement cnst = parse_line_constant(&line.item);
            ct_insert(ct, cnst);

        } else {
            fprintf(stderr, "Error: in `parse_linizer` unknown line [%lu] type\n", i + 1);
            exit(1);
        }
    }

    // TODO: erros for unused deffred labels
    for (size_t i = 0; i < PJ->deferred.count; ++i) {
        Label dlabel = PJ->deferred.labels[i];
        Label label = ll_search_label(&PJ->current, dlabel.name);
        if (label.addr != (uint64_t)(-1)) {
            block_chain.items[dlabel.addr].items[1] = OBJ_UINT(label.addr); 
        }
    }

    Object_Block entry_obj = {0};
    objb_push(arena, &entry_obj, OBJ_UINT(entry_ip));
    block_chain_push(arena, &block_chain, entry_obj);

    if (bc_debug == BLOCK_CHAIN_DEBUG_TRUE) 
        block_chain_debug(&block_chain);
    
    return block_chain;
}

void objb_to_cpu(Arena *arena, CPU *c, Object_Block *objb)
{
    for (size_t i = 0; i < objb->count; ++i) {
        Object obj = objb->items[i];
        if (c->program_size + 1 >= c->program_capacity) {
            size_t old_size = c->program_capacity * sizeof(*c->program);
            c->program_capacity *= 2;
            c->program = arena_realloc(arena, c->program, old_size,
                                       c->program_capacity * sizeof(*c->program));
        }
        c->program[c->program_size++] = obj;
    }
}

void block_chain_to_cpu(Arena *arena, CPU *c, Block_Chain *block_chain)
{
    if (c->program_capacity == 0) {
        c->program_capacity = PROGRAM_INIT_CAPACITY;
        c->program = arena_alloc(arena, c->program_capacity * sizeof(*c->program));
        c->program_size = 0;
    }

    for (size_t i = 0; i < block_chain->count; ++i) {
        Object_Block objb = block_chain->items[i];
        objb_to_cpu(arena, c, &objb);
    }
}

int translate_inst(String_View inst_sv, Hash_Table *ht)
{
    if (ht->capacity == 0) {
        fprintf(stderr, "Error: in `translate_inst` ht is not init\n");
        exit(1);
    }
    char *inst_cstr = sv_to_cstr(inst_sv);
    int inst = ht_get_inst(ht, inst_cstr);
    free(inst_cstr);

    return inst;
}

int parse_register(String_View sv)
{   
    char *acc = reg_as_cstr(ACC);
    if (sv_cmp(sv, sv_from_cstr(acc)))
        return ACC;

    char *accf = reg_as_cstr(ACCF);
    if (sv_cmp(sv, sv_from_cstr(accf)))
        return ACCF;

    int n = sv.data[1] - '0';

    if (sv.data[0] == 'r' && isdigit(sv.data[1])) {
        return n;

    } else if (sv.data[0] == 'f' && isdigit(sv.data[1])) {
        n = n + ACC + 1;
        return n;

    } else {
        return -1;
    }
}