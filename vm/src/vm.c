/*
The MIT License (MIT)

Copyright (c) 2015 Terence Parr, Hanzhou Shi, Shuai Yuan, Yuanyuan Zhang

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wich.h>

#include <gc.h>
#include "vm.h"

#include "wloader.h"

VM_INSTRUCTION vm_instructions[] = {
        {"HALT", HALT, 0},

        {"IADD", IADD, 0},
        {"ISUB", ISUB, 0},
        {"IMUL", IMUL, 0},
        {"IDIV", IDIV, 0},
        {"FADD", FADD, 0},
        {"FSUB", FSUB, 0},
        {"FMUL", FMUL, 0},
        {"FDIV", FDIV, 0},

        {"OR", OR, 0},
        {"AND", AND, 0},
        {"INEG", INEG, 0},
        {"FNEG", FNEG, 0},
        {"NOT", NOT, 0},

		{"I2F", I2F, 0},
        {"F2I", F2I, 0},
		{"I2S", I2S, 0},
		{"F2S", F2S, 0},
		{"V2S", V2S, 0},
		{"F2V", F2V, 0},

        {"IEQ", IEQ, 0},
        {"INEQ", INEQ, 0},
        {"ILT", ILT, 0},
        {"ILE", ILE, 0},
        {"IGT", IGT, 0},
        {"IGE", IGE, 0},
        {"FEQ", FEQ, 0},
        {"FNEQ", FNEQ, 0},
        {"FLT", FLT, 0},
        {"FLE", FLE, 0},
        {"FGT", FGT, 0},
        {"FGE", FGE, 0},
        {"ISNIL", ISNIL, 0},

        {"BR",  BR, 2},
        {"BRT", BRT, 2},
        {"BRF", BRF, 2},
        {"ICONST", ICONST, 4},
        {"FCONST", FCONST, 4},
        {"SCONST", SCONST, 2},
        {"ILOAD", ILOAD, 2},
        {"FLOAD", FLOAD, 2},
        {"VLOAD", VLOAD, 2},
        {"SLOAD", SLOAD, 2},
        {"STORE", STORE, 2},
        {"VECTOR", VECTOR, 0},
        {"LOAD_INDEX", LOAD_INDEX, 0},
        {"STORE_INDEX", STORE_INDEX, 0},
        {"NIL", NIL, 0},
        {"POP", POP, 1},
        {"CALL", CALL, 2},
        {"RETV", RETV, 0},
        {"RET", RET, 0},
        {"IPRINT", IPRINT, 0},
        {"FPRINT", FPRINT, 0},
		{"BPRINT", BPRINT, 0},
		{"SPRINT", SPRINT, 0},
        {"VPRINT", VPRINT, 0},
        {"NOP", NOP, 0}
};

static void vm_print_instr(VM *vm, addr32 ip);
static void vm_print_stack(VM *vm);
static void vm_print_data(VM *vm, addr32 address, int length);
static inline int int32(const byte *data, addr32 ip);
static inline int int16(const byte *data, addr32 ip);
static inline float float32(const byte *data, addr32 ip);
static void vm_call(VM *vm, Function_metadata *func);
static void vm_print_stack_value(VM *vm, word p);

VM *vm_alloc()
{
    VM *vm = calloc(1, sizeof(VM));
    return vm;
}

void vm_init(VM *vm, byte *code, int code_size)
{
	// we are linking in mark-and-compact collector so allocations all occur outside of the VM
    vm->code = code;
    vm->code_size = code_size;
    vm->sp = -1; // grow upwards, stack[sp] is top of stack and valid
    vm->fp = -1; // frame pointer is invalid initially
    vm->callsp = -1;
}

int def_function(VM *vm, char *name, int return_type, addr32 address, int nargs, int nlocals)
{
    if ( vm->num_functions>=MAX_FUNCTIONS ) {
        fprintf(stderr, "Exceeded max functions %d\n", MAX_FUNCTIONS);
        return -1;
    }
    int i = vm->num_functions++;
    Function_metadata *f = &vm->functions[i];
    f->name = strdup(name);
    f->return_type = return_type;
    f->address = address;
    f->nargs = nargs;
    f->nlocals = nlocals;
    return i;
}

#define WRITE_BACK_REGISTERS(vm) vm->ip = ip; vm->sp = sp; vm->fp = fp;
#define LOAD_REGISTERS(vm) ip = vm->ip; sp = vm->sp; fp = vm->fp;

static void inline validate_stack_address(int a) {
	if ((a) < 0 || (a) >= MAX_OPND_STACK) {
		fprintf(stderr, "%d stack ptr out of range 0..%d\n", a, MAX_OPND_STACK - 1);
	}
}

#define valid_array_index(arr,i) \
    if ( i<0 || i>=arr->length ) {fprintf(stderr, "index %d out of bounds 0..%d\n", i, arr->length);}

#define push(w) \
    validate_stack_address(sp+1);\
    stack[++sp] = (word)w; \

void vm_exec(VM *vm, bool trace)
{
	int a = 0;
	word aver = 0;
	word avec = 0;
    word b = 0;
    int i = 0;
	bool b1, b2;
	double f,g;
    word address = 0;
    PVector_ptr vptr;
    int x, y;
    Activation_Record *frame;

    // simulate a call to main()
    Function_metadata *const main = vm_function(vm, "main");
    vm_call(vm, main);

    // Define VM registers (C compiler probably ignores 'register' nowadays
    // but it's good documentation in this case. Keep as locals for
    // convenience but write them back to the vm object after each decode/execute.
    register addr32 ip = vm->ip;
    register int sp = vm->sp;
    register int fp = vm->fp;
    const byte *code = vm->code;
	element *stack = vm->stack;

    int opcode = code[ip];

    while (opcode != HALT && ip < vm->code_size ) {
        if (trace) vm_print_instr(vm, ip);
        ip++; //jump to next instruction or to operand
        switch (opcode) {
            case IADD:
                validate_stack_address(sp-1);
                y = stack[sp--].i;
                x = stack[sp].i;
                stack[sp].i = x + y;
                break;
            case ISUB:
                validate_stack_address(sp-1);
				y = stack[sp--].i;
				x = stack[sp].i;
				stack[sp].i = x - y;
                break;
            case IMUL:
				y = stack[sp--].i;
				x = stack[sp].i;
				stack[sp].i = x * y;
                break;
            case IDIV:
				y = stack[sp--].i;
				x = stack[sp].i;
				stack[sp].i = x / y;
                break;
            case FADD:
				f = stack[sp--].f;
				g = stack[sp].f;
				stack[sp].f = f + g;
                break;
            case FSUB:
                validate_stack_address(sp-1);
				f = stack[sp--].f;
				g = stack[sp].f;
				stack[sp].f = f - g;
                break;
            case FMUL:
                validate_stack_address(sp-1);
				f = stack[sp--].f;
				g = stack[sp].f;
				stack[sp].f = f * g;
                break;
            case FDIV:
                validate_stack_address(sp-1);
				f = stack[sp--].f;
				g = stack[sp].f;
				stack[sp].f = f / g;
                break;
            case OR :
                validate_stack_address(sp-1);
                b2 = stack[sp--].b;
                b1 = stack[sp].b;
                stack[sp].b = b1 || b2;
                break;
            case AND :
				validate_stack_address(sp-1);
				b2 = stack[sp--].b;
				b1 = stack[sp].b;
				stack[sp].b = b1 && b2;
                break;
            case INEG:
                validate_stack_address(sp);
                stack[sp].i = -stack[sp].i;
                break;
            case FNEG:
                validate_stack_address(sp);
				stack[sp].f = -stack[sp].f;
                break;
            case NOT:
                validate_stack_address(sp);
                stack[sp].b = !stack[sp].b;
                break;
            case I2F:
				validate_stack_address(sp);
				stack[sp].f = stack[sp].i;
				break;
            case F2I:
				validate_stack_address(sp);
				stack[sp].i = (int)stack[sp].f;
				break;
            case IEQ:
                validate_stack_address(sp-1);
				y = stack[sp--].i;
				x = stack[sp].i;
				stack[sp].b = x == y;
                break;
            case INEQ:
                validate_stack_address(sp-1);
				y = stack[sp--].i;
				x = stack[sp].i;
				stack[sp].b = x != y;
                break;
            case ILT:
                validate_stack_address(sp-1);
				y = stack[sp--].i;
				x = stack[sp].i;
				stack[sp].b = x < y;
                break;
            case ILE:
                validate_stack_address(sp-1);
				y = stack[sp--].i;
				x = stack[sp].i;
				stack[sp].b = x <= y;
                break;
            case IGT:
                validate_stack_address(sp-1);
				y = stack[sp--].i;
				x = stack[sp].i;
				stack[sp].b = x > y;
                break;
            case IGE:
                validate_stack_address(sp-1);
				y = stack[sp--].i;
				x = stack[sp].i;
				stack[sp].b = x >= y;
                break;
            case FEQ:
                validate_stack_address(sp-1);
				g = stack[sp--].f;
				f = stack[sp].f;
                stack[sp].b = f == g;
                break;
            case FNEQ:
                validate_stack_address(sp-1);
				g = stack[sp--].f;
				f = stack[sp].f;
				stack[sp].b = f != g;
                break;
            case FLT:
                validate_stack_address(sp-1);
				g = stack[sp--].f;
				f = stack[sp].f;
				stack[sp].b = f < g;
                break;
            case FLE:
                validate_stack_address(sp-1);
				g = stack[sp--].f;
				f = stack[sp].f;
				stack[sp].b = f <= g;
                break;
            case FGT:
                validate_stack_address(sp-1);
				g = stack[sp--].f;
				f = stack[sp].f;
				stack[sp].b = f > g;
                break;
            case FGE:
                validate_stack_address(sp-1);
				g = stack[sp--].f;
				f = stack[sp].f;
				stack[sp].b = f >= g;
                break;
            case ISNIL:
                validate_stack_address(sp);
                stack[sp].b = false; // = stack[sp]== VM_NIL ? VM_FALSE : VM_TRUE;
                break;
            case BR:
                ip += int16(code,ip) - 1;
                break;
            case BRT:
                validate_stack_address(sp);
                if ( stack[sp--].b ) {
                    int offset = int16(code,ip);
                    ip += offset - 1;
                }
                else {
                    ip += 2;
                }
                break;
            case BRF:
                validate_stack_address(sp);
                if ( !stack[sp--].b ) {
                    int offset = int16(code,ip);
                    ip += offset - 1;
                }
                else {
                    ip += 2;
                }
                break;
            case ICONST: // code4[ip]
				stack[++sp].i = int32(code,ip);
				ip += 4;
                break;
            case FCONST:
				stack[++sp].f = float32(code,ip);
				ip += 4;
                break;
            case SCONST :
                i = int16(code,ip);
                ip += 2;
				stack[++sp].s = vm->strings[i];
                break;
            case ILOAD:
				i = int16(code,ip); // get index into locals
				ip += 2;
				stack[++sp].i = vm->call_stack[vm->callsp].locals[i].i;
				break;
            case FLOAD:
                i = int16(code,ip); // get index into locals
                ip += 2;
				stack[++sp].f = vm->call_stack[vm->callsp].locals[i].f;
                break;
            case STORE:
                i = int16(code,ip);
                ip += 2;
                vm->call_stack[vm->callsp].locals[i] = stack[sp--]; // untyped store; it'll just copy all bits
                break;
            case VECTOR:
				i = stack[sp--].i;
				PVector_ptr pvec = PVector_init(0, i);
				stack[++sp].vptr = pvec;
                break;
            case LOAD_INDEX:
                validate_stack_address(sp-1);
				i = stack[sp--].i;
				vptr = stack[sp--].vptr;
				vm->stack[++sp].f = ith(vptr, i);
                break;
            case STORE_INDEX:
                validate_stack_address(sp-2);
				f = stack[sp--].f;
				i = stack[sp--].i;
				vptr = stack[sp--].vptr;
				set_ith(vptr, i, f);
                break;
            case NIL:
//                push(0);
                break;
            case POP:
                sp--;
                break;
            case CALL:
                a = int16(code,ip); // load index of function from code memory
                WRITE_BACK_REGISTERS(vm); // (ip has been updated)
                vm_call(vm, &vm->functions[a]);
                LOAD_REGISTERS(vm);
                break;
			case RETV:
                //element retv = stack[sp--];  // pop return value
				// TODO: can't we just leave on opnd stack and return?
				break;
            case RET:
                frame = &vm->call_stack[vm->callsp--];
                ip = frame->retaddr;
                fprintf(stderr, "returning from %s to %d\n", frame->func->name, ip);
                break;
            case IPRINT:
                validate_stack_address(sp);
                printf("%d\n", stack[sp--].i);
                break;
			case FPRINT:
				validate_stack_address(sp);
				printf("%f\n", stack[sp--].f);
				break;
			case BPRINT:
				validate_stack_address(sp);
				printf("%s\n", stack[sp--].b ? "true" : "false");
				break;
			case SPRINT:
				validate_stack_address(sp);
				printf("%s\n", stack[sp--].s);
				break;
			case VPRINT:
				validate_stack_address(sp);
				print_vector(stack[sp--].vptr);
				break;
            case NOP : break;
            default:
                printf("invalid opcode: %d at ip=%d\n", opcode, (ip - 1));
                exit(1);
        }
        WRITE_BACK_REGISTERS(vm);
        if (trace) vm_print_stack(vm);
        opcode = code[ip];
    }
    if (trace) vm_print_instr(vm, ip);
    if (trace) vm_print_stack(vm);
//    if (trace) vm_print_data(vm, 0, vm->data_size+vm->heap_size);
}

void vm_push(VM *vm, element value)
{
    if (vm->sp >= -1) vm->stack[++vm->sp] = value; \
    else fprintf(stderr, "whoa. sp < -1");
}

void vm_call(VM *vm, Function_metadata *func)
{
    fprintf(stderr, "call %s\n", func->name);
	Activation_Record *r = &vm->call_stack[++vm->callsp];
    r->func = func;
    r->retaddr = vm->ip + 2; // save return address (assume ip is 1st byte of operand)
    // copy args to frame activation record
    for (int i = func->nargs-1; i>=0 ; --i) {
        r->locals[i] = vm->stack[vm->sp--];
    }
    for (int i = 0; i<func->nlocals; i++) {
        r->locals[func->nargs+i].i = 0; // init locals
    }
    vm->ip = func->address; // jump!
}

static inline int int32(const byte *data, addr32 ip)
{
    return *((word32 *)&data[ip]);
}

static inline float float32(const byte *data, addr32 ip)
{
    return *((int *)&data[ip]); // could be negative value
}

static inline int int16(const byte *data, addr32 ip)
{
    return *((short *)&data[ip]); // could be negative value
}

static void vm_print_instr(VM *vm, addr32 ip)
{
    int opcode = vm->code[ip];
    VM_INSTRUCTION *inst = &vm_instructions[opcode];
    switch (inst->opnd_size) {
        case 0:
            fprintf(stderr, "%04d:  %-25s", ip, inst->name);
            break;
        case 1:
            fprintf(stderr, "%04d:  %-15s%-10d", ip, inst->name, vm->code[ip+1]);
            break;
        case 2:
            fprintf(stderr, "%04d:  %-15s%-10d", ip, inst->name, int16(vm->code, ip + 1));
            break;
        case 4:
            fprintf(stderr, "%04d:  %-15s%-10d", ip, inst->name, int32(vm->code, ip + 1));
            break;
    }
}

static void vm_print_stack(VM *vm) {
    // stack grows upwards; stack[sp] is top of stack
    fprintf(stderr, "calls=[");
    for (int i = 0; i <= vm->callsp; i++) {
        Activation_Record *frame = &vm->call_stack[i];
        Function_metadata *func = frame->func;
        fprintf(stderr, " %s=[", func->name);
        for (int j = 0; j < func->nlocals+func->nargs; ++j) {
            vm_print_stack_value(vm, frame->locals[j].i);
        }
        fprintf(stderr, " ]");
    }
    fprintf(stderr, " ]  ");
    fprintf(stderr, "opnds=[");
    for (int i = 0; i <= vm->sp; i++) {
        word p = vm->stack[i].i;
        vm_print_stack_value(vm, p);
    }
    fprintf(stderr, " ] fp=%d sp=%d\n", vm->fp, vm->sp);
}

void vm_print_stack_value(VM *vm, word p) {
	if ( ((long)p) >= 0 ) {
		fprintf(stderr, " %lu", p);
	}
	else {
		fprintf(stderr, " %ld", p); // assume negative value is correct (and not a huge unsigned)
	}
}
