// in principle, rt_xxx functions are called only by vm/native/viper and make assumptions about args
// mp_xxx functions are safer and can be called by anyone
// note that rt_assign_xxx are called only from emit*, and maybe we can rename them to reflect this

#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "nlr.h"
#include "misc.h"
#include "mpconfig.h"
#include "mpqstr.h"
#include "obj.h"
#include "runtime0.h"
#include "runtime.h"
#include "map.h"
#include "builtin.h"

#if 0 // print debugging info
#define DEBUG_PRINT (1)
#define WRITE_CODE (1)
#define DEBUG_printf(args...) printf(args)
#define DEBUG_OP_printf(args...) printf(args)
#else // don't print debugging info
#define DEBUG_printf(args...) (void)0
#define DEBUG_OP_printf(args...) (void)0
#endif

// locals and globals need to be pointers because they can be the same in outer module scope
static mp_map_t *map_locals;
static mp_map_t *map_globals;
static mp_map_t map_builtins;

typedef enum {
    MP_CODE_NONE,
    MP_CODE_BYTE,
    MP_CODE_NATIVE,
    MP_CODE_INLINE_ASM,
} mp_code_kind_t;

typedef struct _mp_code_t {
    mp_code_kind_t kind;
    int n_args;
    int n_locals;
    int n_stack;
    bool is_generator;
    union {
        struct {
            byte *code;
            uint len;
        } u_byte;
        struct {
            mp_fun_t fun;
        } u_native;
        struct {
            void *fun;
        } u_inline_asm;
    };
} mp_code_t;

static int next_unique_code_id;
static machine_uint_t unique_codes_alloc = 0;
static mp_code_t *unique_codes = NULL;

#ifdef WRITE_CODE
FILE *fp_write_code = NULL;
#endif

// a good optimising compiler will inline this if necessary
static void mp_map_add_qstr(mp_map_t *map, qstr qstr, mp_obj_t value) {
    mp_map_lookup(map, MP_OBJ_NEW_QSTR(qstr), MP_MAP_LOOKUP_ADD_IF_NOT_FOUND)->value = value;
}

void rt_init(void) {
    // locals = globals for outer module (see Objects/frameobject.c/PyFrame_New())
    map_locals = map_globals = mp_map_new(1);
    mp_map_add_qstr(map_globals, MP_QSTR___name__, mp_obj_new_str(MP_QSTR___main__));

    // init built-in hash table
    mp_map_init(&map_builtins, 3);

    // built-in exceptions (TODO, make these proper classes)
    mp_map_add_qstr(&map_builtins, MP_QSTR_AttributeError, mp_obj_new_exception(MP_QSTR_AttributeError));
    mp_map_add_qstr(&map_builtins, MP_QSTR_IndexError, mp_obj_new_exception(MP_QSTR_IndexError));
    mp_map_add_qstr(&map_builtins, MP_QSTR_KeyError, mp_obj_new_exception(MP_QSTR_KeyError));
    mp_map_add_qstr(&map_builtins, MP_QSTR_NameError, mp_obj_new_exception(MP_QSTR_NameError));
    mp_map_add_qstr(&map_builtins, MP_QSTR_TypeError, mp_obj_new_exception(MP_QSTR_TypeError));
    mp_map_add_qstr(&map_builtins, MP_QSTR_SyntaxError, mp_obj_new_exception(MP_QSTR_SyntaxError));
    mp_map_add_qstr(&map_builtins, MP_QSTR_ValueError, mp_obj_new_exception(MP_QSTR_ValueError));
    mp_map_add_qstr(&map_builtins, MP_QSTR_OSError, mp_obj_new_exception(MP_QSTR_OSError));
    mp_map_add_qstr(&map_builtins, MP_QSTR_AssertionError, mp_obj_new_exception(MP_QSTR_AssertionError));

    // built-in objects
    mp_map_add_qstr(&map_builtins, MP_QSTR_Ellipsis, mp_const_ellipsis);

    // built-in core functions
    mp_map_add_qstr(&map_builtins, MP_QSTR___build_class__, (mp_obj_t)&mp_builtin___build_class___obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR___repl_print__, (mp_obj_t)&mp_builtin___repl_print___obj);

    // built-in types
    mp_map_add_qstr(&map_builtins, MP_QSTR_bool, (mp_obj_t)&bool_type);
#if MICROPY_ENABLE_FLOAT
    mp_map_add_qstr(&map_builtins, MP_QSTR_complex, (mp_obj_t)&complex_type);
#endif
    mp_map_add_qstr(&map_builtins, MP_QSTR_dict, (mp_obj_t)&dict_type);
#if MICROPY_ENABLE_FLOAT
    mp_map_add_qstr(&map_builtins, MP_QSTR_float, (mp_obj_t)&float_type);
#endif
    mp_map_add_qstr(&map_builtins, MP_QSTR_int, (mp_obj_t)&int_type);
    mp_map_add_qstr(&map_builtins, MP_QSTR_list, (mp_obj_t)&list_type);
    mp_map_add_qstr(&map_builtins, MP_QSTR_set, (mp_obj_t)&set_type);
    mp_map_add_qstr(&map_builtins, MP_QSTR_tuple, (mp_obj_t)&tuple_type);
    mp_map_add_qstr(&map_builtins, MP_QSTR_type, (mp_obj_t)&mp_const_type);

    // built-in user functions
    mp_map_add_qstr(&map_builtins, MP_QSTR_abs, (mp_obj_t)&mp_builtin_abs_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_all, (mp_obj_t)&mp_builtin_all_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_any, (mp_obj_t)&mp_builtin_any_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_callable, (mp_obj_t)&mp_builtin_callable_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_chr, (mp_obj_t)&mp_builtin_chr_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_divmod, (mp_obj_t)&mp_builtin_divmod_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_hash, (mp_obj_t)&mp_builtin_hash_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_isinstance, (mp_obj_t)&mp_builtin_isinstance_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_issubclass, (mp_obj_t)&mp_builtin_issubclass_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_iter, (mp_obj_t)&mp_builtin_iter_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_len, (mp_obj_t)&mp_builtin_len_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_max, (mp_obj_t)&mp_builtin_max_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_min, (mp_obj_t)&mp_builtin_min_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_next, (mp_obj_t)&mp_builtin_next_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_ord, (mp_obj_t)&mp_builtin_ord_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_pow, (mp_obj_t)&mp_builtin_pow_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_print, (mp_obj_t)&mp_builtin_print_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_range, (mp_obj_t)&mp_builtin_range_obj);
    mp_map_add_qstr(&map_builtins, MP_QSTR_sum, (mp_obj_t)&mp_builtin_sum_obj);

    next_unique_code_id = 1; // 0 indicates "no code"
    unique_codes_alloc = 0;
    unique_codes = NULL;

#ifdef WRITE_CODE
    fp_write_code = fopen("out-code", "wb");
#endif
}

void rt_deinit(void) {
    m_del(mp_code_t, unique_codes, unique_codes_alloc);
#ifdef WRITE_CODE
    if (fp_write_code != NULL) {
        fclose(fp_write_code);
    }
#endif
}

int rt_get_unique_code_id(void) {
    return next_unique_code_id++;
}

static void alloc_unique_codes(void) {
    if (next_unique_code_id > unique_codes_alloc) {
        // increase size of unique_codes table
        unique_codes = m_renew(mp_code_t, unique_codes, unique_codes_alloc, next_unique_code_id);
        for (int i = unique_codes_alloc; i < next_unique_code_id; i++) {
            unique_codes[i].kind = MP_CODE_NONE;
        }
        unique_codes_alloc = next_unique_code_id;
    }
}

void rt_assign_byte_code(int unique_code_id, byte *code, uint len, int n_args, int n_locals, int n_stack, bool is_generator) {
    alloc_unique_codes();

    assert(1 <= unique_code_id && unique_code_id < next_unique_code_id && unique_codes[unique_code_id].kind == MP_CODE_NONE);
    unique_codes[unique_code_id].kind = MP_CODE_BYTE;
    unique_codes[unique_code_id].n_args = n_args;
    unique_codes[unique_code_id].n_locals = n_locals;
    unique_codes[unique_code_id].n_stack = n_stack;
    unique_codes[unique_code_id].is_generator = is_generator;
    unique_codes[unique_code_id].u_byte.code = code;
    unique_codes[unique_code_id].u_byte.len = len;

    //printf("byte code: %d bytes\n", len);

#ifdef DEBUG_PRINT
    DEBUG_printf("assign byte code: id=%d code=%p len=%u n_args=%d\n", unique_code_id, code, len, n_args);
    for (int i = 0; i < 128 && i < len; i++) {
        if (i > 0 && i % 16 == 0) {
            DEBUG_printf("\n");
        }
        DEBUG_printf(" %02x", code[i]);
    }
    DEBUG_printf("\n");
#if MICROPY_SHOW_BC
    extern void mp_show_byte_code(const byte *code, int len);
    mp_show_byte_code(code, len);
#endif

#ifdef WRITE_CODE
    if (fp_write_code != NULL) {
        fwrite(code, len, 1, fp_write_code);
        fflush(fp_write_code);
    }
#endif
#endif
}

void rt_assign_native_code(int unique_code_id, void *fun, uint len, int n_args) {
    alloc_unique_codes();

    assert(1 <= unique_code_id && unique_code_id < next_unique_code_id && unique_codes[unique_code_id].kind == MP_CODE_NONE);
    unique_codes[unique_code_id].kind = MP_CODE_NATIVE;
    unique_codes[unique_code_id].n_args = n_args;
    unique_codes[unique_code_id].n_locals = 0;
    unique_codes[unique_code_id].n_stack = 0;
    unique_codes[unique_code_id].is_generator = false;
    unique_codes[unique_code_id].u_native.fun = fun;

    //printf("native code: %d bytes\n", len);

#ifdef DEBUG_PRINT
    DEBUG_printf("assign native code: id=%d fun=%p len=%u n_args=%d\n", unique_code_id, fun, len, n_args);
    byte *fun_data = (byte*)(((machine_uint_t)fun) & (~1)); // need to clear lower bit in case it's thumb code
    for (int i = 0; i < 128 && i < len; i++) {
        if (i > 0 && i % 16 == 0) {
            DEBUG_printf("\n");
        }
        DEBUG_printf(" %02x", fun_data[i]);
    }
    DEBUG_printf("\n");

#ifdef WRITE_CODE
    if (fp_write_code != NULL) {
        fwrite(fun_data, len, 1, fp_write_code);
        fflush(fp_write_code);
    }
#endif
#endif
}

void rt_assign_inline_asm_code(int unique_code_id, void *fun, uint len, int n_args) {
    alloc_unique_codes();

    assert(1 <= unique_code_id && unique_code_id < next_unique_code_id && unique_codes[unique_code_id].kind == MP_CODE_NONE);
    unique_codes[unique_code_id].kind = MP_CODE_INLINE_ASM;
    unique_codes[unique_code_id].n_args = n_args;
    unique_codes[unique_code_id].n_locals = 0;
    unique_codes[unique_code_id].n_stack = 0;
    unique_codes[unique_code_id].is_generator = false;
    unique_codes[unique_code_id].u_inline_asm.fun = fun;

#ifdef DEBUG_PRINT
    DEBUG_printf("assign inline asm code: id=%d fun=%p len=%u n_args=%d\n", unique_code_id, fun, len, n_args);
    byte *fun_data = (byte*)(((machine_uint_t)fun) & (~1)); // need to clear lower bit in case it's thumb code
    for (int i = 0; i < 128 && i < len; i++) {
        if (i > 0 && i % 16 == 0) {
            DEBUG_printf("\n");
        }
        DEBUG_printf(" %02x", fun_data[i]);
    }
    DEBUG_printf("\n");

#ifdef WRITE_CODE
    if (fp_write_code != NULL) {
        fwrite(fun_data, len, 1, fp_write_code);
    }
#endif
#endif
}

int rt_is_true(mp_obj_t arg) {
    DEBUG_OP_printf("is true %p\n", arg);
    if (MP_OBJ_IS_SMALL_INT(arg)) {
        if (MP_OBJ_SMALL_INT_VALUE(arg) == 0) {
            return 0;
        } else {
            return 1;
        }
    } else if (arg == mp_const_none) {
        return 0;
    } else if (arg == mp_const_false) {
        return 0;
    } else if (arg == mp_const_true) {
        return 1;
    } else {
        assert(0);
        return 0;
    }
}

mp_obj_t rt_list_append(mp_obj_t self_in, mp_obj_t arg) {
    return mp_obj_list_append(self_in, arg);
}

#define PARSE_DEC_IN_INTG (1)
#define PARSE_DEC_IN_FRAC (2)
#define PARSE_DEC_IN_EXP  (3)

mp_obj_t rt_load_const_dec(qstr qstr) {
#if MICROPY_ENABLE_FLOAT
    DEBUG_OP_printf("load '%s'\n", qstr_str(qstr));
    const char *s = qstr_str(qstr);
    int in = PARSE_DEC_IN_INTG;
    mp_float_t dec_val = 0;
    bool exp_neg = false;
    int exp_val = 0;
    int exp_extra = 0;
    bool imag = false;
    for (; *s; s++) {
        int dig = *s;
        if ('0' <= dig && dig <= '9') {
            dig -= '0';
            if (in == PARSE_DEC_IN_EXP) {
                exp_val = 10 * exp_val + dig;
            } else {
                dec_val = 10 * dec_val + dig;
                if (in == PARSE_DEC_IN_FRAC) {
                    exp_extra -= 1;
                }
            }
        } else if (in == PARSE_DEC_IN_INTG && dig == '.') {
            in = PARSE_DEC_IN_FRAC;
        } else if (in != PARSE_DEC_IN_EXP && (dig == 'E' || dig == 'e')) {
            in = PARSE_DEC_IN_EXP;
            if (s[1] == '+') {
                s++;
            } else if (s[1] == '-') {
                s++;
                exp_neg = true;
            }
        } else if (dig == 'J' || dig == 'j') {
            s++;
            imag = true;
            break;
        } else {
            // unknown character
            break;
        }
    }
    if (*s != 0) {
        nlr_jump(mp_obj_new_exception_msg(MP_QSTR_SyntaxError, "invalid syntax for number"));
    }
    if (exp_neg) {
        exp_val = -exp_val;
    }
    exp_val += exp_extra;
    for (; exp_val > 0; exp_val--) {
        dec_val *= 10;
    }
    for (; exp_val < 0; exp_val++) {
        dec_val *= 0.1;
    }
    if (imag) {
        return mp_obj_new_complex(0, dec_val);
    } else {
        return mp_obj_new_float(dec_val);
    }
#else
    nlr_jump(mp_obj_new_exception_msg(MP_QSTR_SyntaxError, "decimal numbers not supported"));
#endif
}

mp_obj_t rt_load_const_str(qstr qstr) {
    DEBUG_OP_printf("load '%s'\n", qstr_str(qstr));
    return mp_obj_new_str(qstr);
}

mp_obj_t rt_load_name(qstr qstr) {
    // logic: search locals, globals, builtins
    DEBUG_OP_printf("load name %s\n", qstr_str(qstr));
    mp_map_elem_t *elem = mp_map_lookup(map_locals, MP_OBJ_NEW_QSTR(qstr), MP_MAP_LOOKUP);
    if (elem == NULL) {
        elem = mp_map_lookup(map_globals, MP_OBJ_NEW_QSTR(qstr), MP_MAP_LOOKUP);
        if (elem == NULL) {
            elem = mp_map_lookup(&map_builtins, MP_OBJ_NEW_QSTR(qstr), MP_MAP_LOOKUP);
            if (elem == NULL) {
                nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_NameError, "name '%s' is not defined", qstr_str(qstr)));
            }
        }
    }
    return elem->value;
}

mp_obj_t rt_load_global(qstr qstr) {
    // logic: search globals, builtins
    DEBUG_OP_printf("load global %s\n", qstr_str(qstr));
    mp_map_elem_t *elem = mp_map_lookup(map_globals, MP_OBJ_NEW_QSTR(qstr), MP_MAP_LOOKUP);
    if (elem == NULL) {
        elem = mp_map_lookup(&map_builtins, MP_OBJ_NEW_QSTR(qstr), MP_MAP_LOOKUP);
        if (elem == NULL) {
            nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_NameError, "name '%s' is not defined", qstr_str(qstr)));
        }
    }
    return elem->value;
}

mp_obj_t rt_load_build_class(void) {
    DEBUG_OP_printf("load_build_class\n");
    mp_map_elem_t *elem = mp_map_lookup(&map_builtins, MP_OBJ_NEW_QSTR(MP_QSTR___build_class__), MP_MAP_LOOKUP);
    if (elem == NULL) {
        nlr_jump(mp_obj_new_exception_msg(MP_QSTR_NameError, "name '__build_class__' is not defined"));
    }
    return elem->value;
}

mp_obj_t rt_get_cell(mp_obj_t cell) {
    return mp_obj_cell_get(cell);
}

void rt_set_cell(mp_obj_t cell, mp_obj_t val) {
    mp_obj_cell_set(cell, val);
}

void rt_store_name(qstr qstr, mp_obj_t obj) {
    DEBUG_OP_printf("store name %s <- %p\n", qstr_str(qstr), obj);
    mp_map_lookup(map_locals, MP_OBJ_NEW_QSTR(qstr), MP_MAP_LOOKUP_ADD_IF_NOT_FOUND)->value = obj;
}

void rt_store_global(qstr qstr, mp_obj_t obj) {
    DEBUG_OP_printf("store global %s <- %p\n", qstr_str(qstr), obj);
    mp_map_lookup(map_globals, MP_OBJ_NEW_QSTR(qstr), MP_MAP_LOOKUP_ADD_IF_NOT_FOUND)->value = obj;
}

mp_obj_t rt_unary_op(int op, mp_obj_t arg) {
    DEBUG_OP_printf("unary %d %p\n", op, arg);
    if (MP_OBJ_IS_SMALL_INT(arg)) {
        mp_small_int_t val = MP_OBJ_SMALL_INT_VALUE(arg);
        switch (op) {
            case RT_UNARY_OP_NOT: if (val != 0) { return mp_const_true;} else { return mp_const_false; }
            case RT_UNARY_OP_POSITIVE: break;
            case RT_UNARY_OP_NEGATIVE: val = -val; break;
            case RT_UNARY_OP_INVERT: val = ~val; break;
            default: assert(0); val = 0;
        }
        if (MP_OBJ_FITS_SMALL_INT(val)) {
            return MP_OBJ_NEW_SMALL_INT(val);
        }
        return mp_obj_new_int(val);
    } else { // will be an object (small ints are caught in previous if)
        mp_obj_base_t *o = arg;
        if (o->type->unary_op != NULL) {
            mp_obj_t result = o->type->unary_op(op, arg);
            if (result != NULL) {
                return result;
            }
        }
        // TODO specify in error message what the operator is
        nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_TypeError, "bad operand type for unary operator: '%s'", o->type->name));
    }
}

mp_obj_t rt_binary_op(int op, mp_obj_t lhs, mp_obj_t rhs) {
    DEBUG_OP_printf("binary %d %p %p\n", op, lhs, rhs);

    // TODO correctly distinguish inplace operators for mutable objects
    // lookup logic that CPython uses for +=:
    //   check for implemented +=
    //   then check for implemented +
    //   then check for implemented seq.inplace_concat
    //   then check for implemented seq.concat
    //   then fail
    // note that list does not implement + or +=, so that inplace_concat is reached first for +=

    // deal with == and != for all types
    if (op == RT_COMPARE_OP_EQUAL || op == RT_COMPARE_OP_NOT_EQUAL) {
        if (mp_obj_equal(lhs, rhs)) {
            if (op == RT_COMPARE_OP_EQUAL) {
                return mp_const_true;
            } else {
                return mp_const_false;
            }
        } else {
            if (op == RT_COMPARE_OP_EQUAL) {
                return mp_const_false;
            } else {
                return mp_const_true;
            }
        }
    }

    // deal with exception_match for all types
    if (op == RT_COMPARE_OP_EXCEPTION_MATCH) {
        // TODO properly! at the moment it just compares the exception identifier for equality
        if (MP_OBJ_IS_TYPE(lhs, &exception_type) && MP_OBJ_IS_TYPE(rhs, &exception_type)) {
            if (mp_obj_exception_get_type(lhs) == mp_obj_exception_get_type(rhs)) {
                return mp_const_true;
            } else {
                return mp_const_false;
            }
        }
    }

    if (MP_OBJ_IS_SMALL_INT(lhs)) {
        mp_small_int_t lhs_val = MP_OBJ_SMALL_INT_VALUE(lhs);
        if (MP_OBJ_IS_SMALL_INT(rhs)) {
            mp_small_int_t rhs_val = MP_OBJ_SMALL_INT_VALUE(rhs);
            switch (op) {
                case RT_BINARY_OP_OR:
                case RT_BINARY_OP_INPLACE_OR: lhs_val |= rhs_val; break;
                case RT_BINARY_OP_XOR:
                case RT_BINARY_OP_INPLACE_XOR: lhs_val ^= rhs_val; break;
                case RT_BINARY_OP_AND:
                case RT_BINARY_OP_INPLACE_AND: lhs_val &= rhs_val; break;
                case RT_BINARY_OP_LSHIFT:
                case RT_BINARY_OP_INPLACE_LSHIFT: lhs_val <<= rhs_val; break;
                case RT_BINARY_OP_RSHIFT:
                case RT_BINARY_OP_INPLACE_RSHIFT: lhs_val >>= rhs_val; break;
                case RT_BINARY_OP_ADD:
                case RT_BINARY_OP_INPLACE_ADD: lhs_val += rhs_val; break;
                case RT_BINARY_OP_SUBTRACT:
                case RT_BINARY_OP_INPLACE_SUBTRACT: lhs_val -= rhs_val; break;
                case RT_BINARY_OP_MULTIPLY:
                case RT_BINARY_OP_INPLACE_MULTIPLY: lhs_val *= rhs_val; break;
                case RT_BINARY_OP_FLOOR_DIVIDE:
                case RT_BINARY_OP_INPLACE_FLOOR_DIVIDE: lhs_val /= rhs_val; break;
    #if MICROPY_ENABLE_FLOAT
                case RT_BINARY_OP_TRUE_DIVIDE:
                case RT_BINARY_OP_INPLACE_TRUE_DIVIDE: return mp_obj_new_float((mp_float_t)lhs_val / (mp_float_t)rhs_val);
    #endif

                // TODO implement modulo as specified by Python
                case RT_BINARY_OP_MODULO:
                case RT_BINARY_OP_INPLACE_MODULO: lhs_val %= rhs_val; break;

                // TODO check for negative power, and overflow
                case RT_BINARY_OP_POWER:
                case RT_BINARY_OP_INPLACE_POWER:
                {
                    int ans = 1;
                    while (rhs_val > 0) {
                        if (rhs_val & 1) {
                            ans *= lhs_val;
                        }
                        lhs_val *= lhs_val;
                        rhs_val /= 2;
                    }
                    lhs_val = ans;
                    break;
                }
                case RT_COMPARE_OP_LESS: return MP_BOOL(lhs_val < rhs_val); break;
                case RT_COMPARE_OP_MORE: return MP_BOOL(lhs_val > rhs_val); break;
                case RT_COMPARE_OP_LESS_EQUAL: return MP_BOOL(lhs_val <= rhs_val); break;
                case RT_COMPARE_OP_MORE_EQUAL: return MP_BOOL(lhs_val >= rhs_val); break;

                default: assert(0);
            }
            // TODO: We just should make mp_obj_new_int() inline and use that
            if (MP_OBJ_FITS_SMALL_INT(lhs_val)) {
                return MP_OBJ_NEW_SMALL_INT(lhs_val);
            }
            return mp_obj_new_int(lhs_val);
        } else if (MP_OBJ_IS_TYPE(rhs, &float_type)) {
            return mp_obj_float_binary_op(op, lhs_val, rhs);
        } else if (MP_OBJ_IS_TYPE(rhs, &complex_type)) {
            return mp_obj_complex_binary_op(op, lhs_val, 0, rhs);
        }
    } else {
        if (MP_OBJ_IS_OBJ(lhs)) {
            mp_obj_base_t *o = lhs;
            if (o->type->binary_op != NULL) {
                mp_obj_t result = o->type->binary_op(op, lhs, rhs);
                if (result != NULL) {
                    return result;
                }
            }
        }
    }

    // TODO specify in error message what the operator is
    nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_TypeError,
        "unsupported operand types for binary operator: '%s', '%s'",
        mp_obj_get_type_str(lhs), mp_obj_get_type_str(rhs)));
}

mp_obj_t rt_make_function_from_id(int unique_code_id) {
    DEBUG_OP_printf("make_function_from_id %d\n", unique_code_id);
    if (unique_code_id < 1 || unique_code_id >= next_unique_code_id) {
        // illegal code id
        return mp_const_none;
    }

    // make the function, depending on the code kind
    mp_code_t *c = &unique_codes[unique_code_id];
    mp_obj_t fun;
    switch (c->kind) {
        case MP_CODE_BYTE:
            fun = mp_obj_new_fun_bc(c->n_args, c->n_locals + c->n_stack, c->u_byte.code);
            break;
        case MP_CODE_NATIVE:
            fun = rt_make_function_n(c->n_args, c->u_native.fun);
            break;
        case MP_CODE_INLINE_ASM:
            fun = mp_obj_new_fun_asm(c->n_args, c->u_inline_asm.fun);
            break;
        default:
            assert(0);
            fun = mp_const_none;
    }

    // check for generator functions and if so wrap in generator object
    if (c->is_generator) {
        fun = mp_obj_new_gen_wrap(c->n_locals, c->n_stack, fun);
    }

    return fun;
}

mp_obj_t rt_make_closure_from_id(int unique_code_id, mp_obj_t closure_tuple) {
    DEBUG_OP_printf("make_closure_from_id %d\n", unique_code_id);
    // make function object
    mp_obj_t ffun = rt_make_function_from_id(unique_code_id);
    // wrap function in closure object
    return mp_obj_new_closure(ffun, closure_tuple);
}

mp_obj_t rt_call_function_0(mp_obj_t fun) {
    return rt_call_function_n(fun, 0, NULL);
}

mp_obj_t rt_call_function_1(mp_obj_t fun, mp_obj_t arg) {
    return rt_call_function_n(fun, 1, &arg);
}

mp_obj_t rt_call_function_2(mp_obj_t fun, mp_obj_t arg1, mp_obj_t arg2) {
    mp_obj_t args[2];
    args[1] = arg1;
    args[0] = arg2;
    return rt_call_function_n(fun, 2, args);
}

// args are in reverse order in the array
mp_obj_t rt_call_function_n(mp_obj_t fun_in, int n_args, const mp_obj_t *args) {
    // TODO improve this: fun object can specify its type and we parse here the arguments,
    // passing to the function arrays of fixed and keyword arguments

    DEBUG_OP_printf("calling function %p(n_args=%d, args=%p)\n", fun_in, n_args, args);

    if (MP_OBJ_IS_SMALL_INT(fun_in)) {
        nlr_jump(mp_obj_new_exception_msg(MP_QSTR_TypeError, "'int' object is not callable"));
    } else {
        mp_obj_base_t *fun = fun_in;
        if (fun->type->call_n != NULL) {
            return fun->type->call_n(fun_in, n_args, args);
        } else {
            nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_TypeError, "'%s' object is not callable", fun->type->name));
        }
    }
}

// args are in reverse order in the array; keyword arguments come first, value then key
// eg: (value1, key1, value0, key0, arg1, arg0)
mp_obj_t rt_call_function_n_kw(mp_obj_t fun_in, uint n_args, uint n_kw, const mp_obj_t *args) {
    // TODO merge this and _n into a single, smarter thing
    DEBUG_OP_printf("calling function %p(n_args=%d, n_kw=%d, args=%p)\n", fun_in, n_args, n_kw, args);

    if (MP_OBJ_IS_SMALL_INT(fun_in)) {
        nlr_jump(mp_obj_new_exception_msg(MP_QSTR_TypeError, "'int' object is not callable"));
    } else {
        mp_obj_base_t *fun = fun_in;
        if (fun->type->call_n_kw != NULL) {
            return fun->type->call_n_kw(fun_in, n_args, n_kw, args);
        } else {
            nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_TypeError, "'%s' object is not callable", fun->type->name));
        }
    }
}

// args contains: arg(n_args-1)  arg(n_args-2)  ...  arg(0)  self/NULL  fun
// if n_args==0 then there are only self/NULL and fun
mp_obj_t rt_call_method_n(uint n_args, const mp_obj_t *args) {
    DEBUG_OP_printf("call method %p(self=%p, n_args=%u)\n", args[n_args + 1], args[n_args], n_args);
    return rt_call_function_n(args[n_args + 1], n_args + ((args[n_args] == NULL) ? 0 : 1), args);
}

// args contains: kw_val(n_kw-1)  kw_key(n_kw-1) ... kw_val(0)  kw_key(0)  arg(n_args-1)  arg(n_args-2)  ...  arg(0)  self/NULL  fun
mp_obj_t rt_call_method_n_kw(uint n_args, uint n_kw, const mp_obj_t *args) {
    uint n = n_args + 2 * n_kw;
    DEBUG_OP_printf("call method %p(self=%p, n_args=%u, n_kw=%u)\n", args[n + 1], args[n], n_args, n_kw);
    return rt_call_function_n_kw(args[n + 1], n_args + ((args[n] == NULL) ? 0 : 1), n_kw, args);
}

// items are in reverse order
mp_obj_t rt_build_tuple(int n_args, mp_obj_t *items) {
    return mp_obj_new_tuple_reverse(n_args, items);
}

// items are in reverse order
mp_obj_t rt_build_list(int n_args, mp_obj_t *items) {
    return mp_obj_new_list_reverse(n_args, items);
}

mp_obj_t rt_build_set(int n_args, mp_obj_t *items) {
    return mp_obj_new_set(n_args, items);
}

mp_obj_t rt_store_set(mp_obj_t set, mp_obj_t item) {
    mp_obj_set_store(set, item);
    return set;
}

// unpacked items are stored in order into the array pointed to by items
void rt_unpack_sequence(mp_obj_t seq_in, uint num, mp_obj_t *items) {
    if (MP_OBJ_IS_TYPE(seq_in, &tuple_type) || MP_OBJ_IS_TYPE(seq_in, &list_type)) {
        uint seq_len;
        mp_obj_t *seq_items;
        if (MP_OBJ_IS_TYPE(seq_in, &tuple_type)) {
            mp_obj_tuple_get(seq_in, &seq_len, &seq_items);
        } else {
            mp_obj_list_get(seq_in, &seq_len, &seq_items);
        }
        if (seq_len < num) {
            nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_ValueError, "need more than %d values to unpack", (void*)(machine_uint_t)seq_len));
        } else if (seq_len > num) {
            nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_ValueError, "too many values to unpack (expected %d)", (void*)(machine_uint_t)num));
        }
        memcpy(items, seq_items, num * sizeof(mp_obj_t));
    } else {
        // TODO call rt_getiter and extract via rt_iternext
        nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_TypeError, "'%s' object is not iterable", mp_obj_get_type_str(seq_in)));
    }
}

mp_obj_t rt_build_map(int n_args) {
    return mp_obj_new_dict(n_args);
}

mp_obj_t rt_store_map(mp_obj_t map, mp_obj_t key, mp_obj_t value) {
    // map should always be a dict
    return mp_obj_dict_store(map, key, value);
}

mp_obj_t rt_load_attr(mp_obj_t base, qstr attr) {
    DEBUG_OP_printf("load attr %p.%s\n", base, qstr_str(attr));
    // use load_method
    mp_obj_t dest[2];
    rt_load_method(base, attr, dest);
    if (dest[0] == NULL) {
        // load_method returned just a normal attribute
        return dest[1];
    } else {
        // load_method returned a method, so build a bound method object
        return mp_obj_new_bound_meth(dest[0], dest[1]);
    }
}

void rt_load_method(mp_obj_t base, qstr attr, mp_obj_t *dest) {
    DEBUG_OP_printf("load method %p.%s\n", base, qstr_str(attr));

    // clear output to indicate no attribute/method found yet
    dest[0] = MP_OBJ_NULL;
    dest[1] = MP_OBJ_NULL;

    // get the type
    mp_obj_type_t *type = mp_obj_get_type(base);

    // if this type can do its own load, then call it
    if (type->load_attr != NULL) {
        type->load_attr(base, attr, dest);
    }

    // if nothing found yet, look for built-in and generic names
    if (dest[1] == NULL) {
        if (attr == MP_QSTR___next__ && type->iternext != NULL) {
            dest[1] = (mp_obj_t)&mp_builtin_next_obj;
            dest[0] = base;
        } else {
            // generic method lookup
            // this is a lookup in the object (ie not class or type)
            const mp_method_t *meth = type->methods;
            if (meth != NULL) {
                for (; meth->name != NULL; meth++) {
                    if (strcmp(meth->name, qstr_str(attr)) == 0) {
                        // check if the methods are functions, static or class methods
                        // see http://docs.python.org/3.3/howto/descriptor.html
                        if (MP_OBJ_IS_TYPE(meth->fun, &mp_type_staticmethod)) {
                            // return just the function
                            dest[1] = ((mp_obj_staticmethod_t*)meth->fun)->fun;
                        } else if (MP_OBJ_IS_TYPE(meth->fun, &mp_type_classmethod)) {
                            // return a bound method, with self being the type of this object
                            dest[1] = ((mp_obj_classmethod_t*)meth->fun)->fun;
                            dest[0] = mp_obj_get_type(base);
                        } else {
                            // return a bound method, with self being this object
                            dest[1] = (mp_obj_t)meth->fun;
                            dest[0] = base;
                        }
                        break;
                    }
                }
            }
        }
    }

    if (dest[1] == NULL) {
        // no attribute/method called attr
        // following CPython, we give a more detailed error message for type objects
        if (MP_OBJ_IS_TYPE(base, &mp_const_type)) {
            nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_AttributeError, "type object '%s' has no attribute '%s'", ((mp_obj_type_t*)base)->name, qstr_str(attr)));
        } else {
            nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_AttributeError, "'%s' object has no attribute '%s'", mp_obj_get_type_str(base), qstr_str(attr)));
        }
    }
}

void rt_store_attr(mp_obj_t base, qstr attr, mp_obj_t value) {
    DEBUG_OP_printf("store attr %p.%s <- %p\n", base, qstr_str(attr), value);
    mp_obj_type_t *type = mp_obj_get_type(base);
    if (type->store_attr != NULL) {
        if (type->store_attr(base, attr, value)) {
            return;
        }
    }
    nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_AttributeError, "'%s' object has no attribute '%s'", mp_obj_get_type_str(base), qstr_str(attr)));
}

void rt_store_subscr(mp_obj_t base, mp_obj_t index, mp_obj_t value) {
    DEBUG_OP_printf("store subscr %p[%p] <- %p\n", base, index, value);
    if (MP_OBJ_IS_TYPE(base, &list_type)) {
        // list store
        mp_obj_list_store(base, index, value);
    } else if (MP_OBJ_IS_TYPE(base, &dict_type)) {
        // dict store
        mp_obj_dict_store(base, index, value);
    } else {
        assert(0);
    }
}

mp_obj_t rt_getiter(mp_obj_t o_in) {
    if (MP_OBJ_IS_SMALL_INT(o_in)) {
        nlr_jump(mp_obj_new_exception_msg(MP_QSTR_TypeError, "'int' object is not iterable"));
    } else {
        mp_obj_base_t *o = o_in;
        if (o->type->getiter != NULL) {
            return o->type->getiter(o_in);
        } else {
            nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_TypeError, "'%s' object is not iterable", o->type->name));
        }
    }
}

mp_obj_t rt_iternext(mp_obj_t o_in) {
    if (MP_OBJ_IS_SMALL_INT(o_in)) {
        nlr_jump(mp_obj_new_exception_msg(MP_QSTR_TypeError, "'int' object is not an iterator"));
    } else {
        mp_obj_base_t *o = o_in;
        if (o->type->iternext != NULL) {
            return o->type->iternext(o_in);
        } else {
            nlr_jump(mp_obj_new_exception_msg_varg(MP_QSTR_TypeError, "'%s' object is not an iterator", o->type->name));
        }
    }
}

mp_obj_t rt_import_name(qstr name, mp_obj_t fromlist, mp_obj_t level) {
    // build args array
    mp_obj_t args[5];
    args[0] = mp_obj_new_str(name);
    args[1] = mp_const_none; // TODO should be globals
    args[2] = mp_const_none; // TODO should be locals
    args[3] = fromlist;
    args[4] = level; // must be 0; we don't yet support other values

    // TODO lookup __import__ and call that instead of going straight to builtin implementation
    return mp_builtin___import__(5, args);
}

mp_obj_t rt_import_from(mp_obj_t module, qstr name) {
    mp_obj_t x = rt_load_attr(module, name);
    /* TODO convert AttributeError to ImportError
    if (fail) {
        (ImportError, "cannot import name %s", qstr_str(name), NULL)
    }
    */
    return x;
}

mp_map_t *rt_locals_get(void) {
    return map_locals;
}

void rt_locals_set(mp_map_t *m) {
    DEBUG_OP_printf("rt_locals_set(%p)\n", m);
    map_locals = m;
}

mp_map_t *rt_globals_get(void) {
    return map_globals;
}

void rt_globals_set(mp_map_t *m) {
    DEBUG_OP_printf("rt_globals_set(%p)\n", m);
    map_globals = m;
}

// these must correspond to the respective enum
void *const rt_fun_table[RT_F_NUMBER_OF] = {
    rt_load_const_dec,
    rt_load_const_str,
    rt_load_name,
    rt_load_global,
    rt_load_build_class,
    rt_load_attr,
    rt_load_method,
    rt_store_name,
    rt_store_attr,
    rt_store_subscr,
    rt_is_true,
    rt_unary_op,
    rt_build_tuple,
    rt_build_list,
    rt_list_append,
    rt_build_map,
    rt_store_map,
    rt_build_set,
    rt_store_set,
    rt_make_function_from_id,
    rt_call_function_n,
    rt_call_method_n,
    rt_binary_op,
    rt_getiter,
    rt_iternext,
};

/*
void rt_f_vector(rt_fun_kind_t fun_kind) {
    (rt_f_table[fun_kind])();
}
*/
