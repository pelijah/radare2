/* radare2 - LGPL - Copyright 2014-2017 - pancake */

#include <r_anal.h>
#include <r_lib.h>
#include <capstone/capstone.h>
#include <capstone/sparc.h>

#if CS_API_MAJOR < 2
#error Old Capstone not supported
#endif

#define esilprintf(op, fmt, ...) r_strbuf_setf (&op->esil, fmt, ##__VA_ARGS__)
#define INSOP(n) insn->detail->sparc.operands[n]
#define INSCC insn->detail->sparc.cc

static void opex(RStrBuf *buf, csh handle, cs_insn *insn) {
	int i;
	r_strbuf_init (buf);
	r_strbuf_append (buf, "{");
	cs_sparc *x = &insn->detail->sparc;
	r_strbuf_append (buf, "\"operands\":[");
	for (i = 0; i < x->op_count; i++) {
		cs_sparc_op *op = &x->operands[i];
		if (i > 0) {
			r_strbuf_append (buf, ",");
		}
		r_strbuf_append (buf, "{");
		switch (op->type) {
		case SPARC_OP_REG:
			r_strbuf_append (buf, "\"type\":\"reg\"");
			r_strbuf_appendf (buf, ",\"value\":\"%s\"", cs_reg_name (handle, op->reg));
			break;
		case SPARC_OP_IMM:
			r_strbuf_append (buf, "\"type\":\"imm\"");
			r_strbuf_appendf (buf, ",\"value\":%"PFMT64d, op->imm);
			break;
		case SPARC_OP_MEM:
			r_strbuf_append (buf, "\"type\":\"mem\"");
			if (op->mem.base != SPARC_REG_INVALID) {
				r_strbuf_appendf (buf, ",\"base\":\"%s\"", cs_reg_name (handle, op->mem.base));
			}
			r_strbuf_appendf (buf, ",\"disp\":%"PFMT64d"", op->mem.disp);
			break;
		default:
			r_strbuf_append (buf, "\"type\":\"invalid\"");
			break;
		}
		r_strbuf_append (buf, "}");
	}
	r_strbuf_append (buf, "]");
	r_strbuf_append (buf, "}");
}

static int parse_reg_name(RRegItem *reg, csh handle, cs_insn *insn, int reg_num) {
	if (!reg) {
		return -1;
	}
	switch (INSOP (reg_num).type) {
	case SPARC_OP_REG:
		reg->name = (char *)cs_reg_name (handle, INSOP (reg_num).reg);
		break;
	case SPARC_OP_MEM:
		if (INSOP (reg_num).mem.base != SPARC_REG_INVALID) {
			reg->name = (char *)cs_reg_name (handle, INSOP (reg_num).mem.base);
			break;
		}
	default:
		break;
	}
	return 0;
}

static void op_fillval(RAnalOp *op, csh handle, cs_insn *insn) {
	static RRegItem reg;
	switch (op->type & R_ANAL_OP_TYPE_MASK) {
	case R_ANAL_OP_TYPE_LOAD:
		if (INSOP(0).type == SPARC_OP_MEM) {
			ZERO_FILL (reg);
			op->src[0] = r_anal_value_new ();
			op->src[0]->reg = &reg;
			parse_reg_name (op->src[0]->reg, handle, insn, 0);
			op->src[0]->delta = INSOP(0).mem.disp;
		}
		break;
	case R_ANAL_OP_TYPE_STORE:
		if (INSOP(1).type == SPARC_OP_MEM) {
			ZERO_FILL (reg);
			op->dst = r_anal_value_new ();
			op->dst->reg = &reg;
			parse_reg_name (op->dst->reg, handle, insn, 1);
			op->dst->delta = INSOP(1).mem.disp;
		}
		break;
	}
}

static int analop(RAnal *a, RAnalOp *op, ut64 addr, const ut8 *buf, int len, RAnalOpMask mask) {
	static csh handle = 0;
	static int omode;
	cs_insn *insn;
	int mode, n, ret;

	if (!a->big_endian) {
		return -1;
	}

	mode = CS_MODE_LITTLE_ENDIAN;
	if (!strcmp (a->cpu, "v9")) {
		mode |= CS_MODE_V9;
	}
	if (mode != omode) {
		cs_close (&handle);
		handle = 0;
		omode = mode;
	}
	if (handle == 0) {
		ret = cs_open (CS_ARCH_SPARC, mode, &handle);
		if (ret != CS_ERR_OK) {
			return -1;
		}
		cs_option (handle, CS_OPT_DETAIL, CS_OPT_ON);
	}
	op->type = R_ANAL_OP_TYPE_NULL;
	op->size = 0;
	op->delay = 0;
	op->jump = UT64_MAX;
	op->fail = UT64_MAX;
	op->val = UT64_MAX;
	op->ptr = UT64_MAX;
	r_strbuf_init (&op->esil);
	// capstone-next
	n = cs_disasm (handle, (const ut8*)buf, len, addr, 1, &insn);
	if (n < 1) {
		op->type = R_ANAL_OP_TYPE_ILL;
	} else {
		if (mask & R_ANAL_OP_MASK_OPEX) {
			opex (&op->opex, handle, insn);
		}
		op->size = insn->size;
		op->id = insn->id;
		switch (insn->id) {
		case SPARC_INS_INVALID:
			op->type = R_ANAL_OP_TYPE_ILL;
			break;
		case SPARC_INS_MOV:
			op->type = R_ANAL_OP_TYPE_MOV;
			break;
		case SPARC_INS_RETT:
		case SPARC_INS_RET:
		case SPARC_INS_RETL:
			op->type = R_ANAL_OP_TYPE_RET;
			op->delay = 1;
			break;
		case SPARC_INS_UNIMP:
			op->type = R_ANAL_OP_TYPE_UNK;
			break;
		case SPARC_INS_CALL:
			switch (INSOP(0).type) {
			case SPARC_OP_MEM:
				// TODO
				break;
			case SPARC_OP_REG:
				op->type = R_ANAL_OP_TYPE_UCALL;
				op->delay = 1;
				break;
			default:
				op->type = R_ANAL_OP_TYPE_CALL;
				op->delay = 1;
				op->jump = INSOP(0).imm;
				break;
			}
			break;
		case SPARC_INS_NOP:
			op->type = R_ANAL_OP_TYPE_NOP;
			break;
		case SPARC_INS_CMP:
			op->type = R_ANAL_OP_TYPE_CMP;
			break;
		case SPARC_INS_JMP:
		case SPARC_INS_JMPL:
			op->type = R_ANAL_OP_TYPE_JMP;
			op->delay = 1;
			op->jump = INSOP(0).imm;
			break;
		case SPARC_INS_LDD:
		case SPARC_INS_LD:
		case SPARC_INS_LDQ:
		case SPARC_INS_LDSB:
		case SPARC_INS_LDSH:
		case SPARC_INS_LDSW:
		case SPARC_INS_LDUB:
		case SPARC_INS_LDUH:
		case SPARC_INS_LDX:
			op->type = R_ANAL_OP_TYPE_LOAD;
			break;
		case SPARC_INS_STBAR:
		case SPARC_INS_STB:
		case SPARC_INS_STD:
		case SPARC_INS_ST:
		case SPARC_INS_STH:
		case SPARC_INS_STQ:
		case SPARC_INS_STX:
			op->type = R_ANAL_OP_TYPE_STORE;
			break;
		case SPARC_INS_ORCC:
		case SPARC_INS_ORNCC:
		case SPARC_INS_ORN:
		case SPARC_INS_OR:
			op->type = R_ANAL_OP_TYPE_OR;
			break;
		case SPARC_INS_B:
		case SPARC_INS_BMASK:
		case SPARC_INS_BRGEZ:
		case SPARC_INS_BRGZ:
		case SPARC_INS_BRLEZ:
		case SPARC_INS_BRLZ:
		case SPARC_INS_BRNZ:
		case SPARC_INS_BRZ:
		case SPARC_INS_FB:
			switch (INSOP(0).type) {
			case SPARC_OP_REG:
				op->type = R_ANAL_OP_TYPE_CJMP;
				op->delay = 1;
				if (INSCC != SPARC_CC_ICC_N) { // never
					op->jump = INSOP (1).imm;
				}
				if (INSCC != SPARC_CC_ICC_A) { // always
					op->fail = addr + 8;
				}
				break;
			case SPARC_OP_IMM:
				op->type = R_ANAL_OP_TYPE_CJMP;
				op->delay = 1;
				if (INSCC != SPARC_CC_ICC_N) { // never
					op->jump = INSOP (0).imm;
				}
				if (INSCC != SPARC_CC_ICC_A) { // always
					op->fail = addr + 8;
				}
				break;
			default:
				// MEM?
				break;
			}
			break;
		case SPARC_INS_FHSUBD:
		case SPARC_INS_FHSUBS:
		case SPARC_INS_FPSUB16:
		case SPARC_INS_FPSUB16S:
		case SPARC_INS_FPSUB32:
		case SPARC_INS_FPSUB32S:
		case SPARC_INS_FSUBD:
		case SPARC_INS_FSUBQ:
		case SPARC_INS_FSUBS:
		case SPARC_INS_SUBCC:
		case SPARC_INS_SUBX:
		case SPARC_INS_SUBXCC:
		case SPARC_INS_SUB:
		case SPARC_INS_TSUBCCTV:
		case SPARC_INS_TSUBCC:
			op->type = R_ANAL_OP_TYPE_SUB;
			break;
		case SPARC_INS_ADDCC:
		case SPARC_INS_ADDX:
		case SPARC_INS_ADDXCC:
		case SPARC_INS_ADDXC:
		case SPARC_INS_ADDXCCC:
		case SPARC_INS_ADD:
		case SPARC_INS_FADDD:
		case SPARC_INS_FADDQ:
		case SPARC_INS_FADDS:
		case SPARC_INS_FHADDD:
		case SPARC_INS_FHADDS:
		case SPARC_INS_FNADDD:
		case SPARC_INS_FNADDS:
		case SPARC_INS_FNHADDD:
		case SPARC_INS_FNHADDS:
		case SPARC_INS_FPADD16:
		case SPARC_INS_FPADD16S:
		case SPARC_INS_FPADD32:
		case SPARC_INS_FPADD32S:
		case SPARC_INS_FPADD64:
		case SPARC_INS_TADDCCTV:
		case SPARC_INS_TADDCC:
			op->type = R_ANAL_OP_TYPE_ADD;
			break;
		case SPARC_INS_FDMULQ:
		case SPARC_INS_FMUL8SUX16:
		case SPARC_INS_FMUL8ULX16:
		case SPARC_INS_FMUL8X16:
		case SPARC_INS_FMUL8X16AL:
		case SPARC_INS_FMUL8X16AU:
		case SPARC_INS_FMULD:
		case SPARC_INS_FMULD8SUX16:
		case SPARC_INS_FMULD8ULX16:
		case SPARC_INS_FMULQ:
		case SPARC_INS_FMULS:
		case SPARC_INS_FSMULD:
		case SPARC_INS_MULX:
		case SPARC_INS_SMULCC:
		case SPARC_INS_SMUL:
		case SPARC_INS_UMULCC:
		case SPARC_INS_UMULXHI:
		case SPARC_INS_UMUL:
		case SPARC_INS_XMULX:
		case SPARC_INS_XMULXHI:
			op->type = R_ANAL_OP_TYPE_MUL;
			break;
		case SPARC_INS_FDIVD:
		case SPARC_INS_FDIVQ:
		case SPARC_INS_FDIVS:
		case SPARC_INS_SDIVCC:
		case SPARC_INS_SDIVX:
		case SPARC_INS_SDIV:
		case SPARC_INS_UDIVCC:
		case SPARC_INS_UDIVX:
		case SPARC_INS_UDIV:
			op->type = R_ANAL_OP_TYPE_DIV;
			break;
		}
		if (mask & R_ANAL_OP_MASK_VAL) {
			op_fillval (op, handle, insn);
		}
		cs_free (insn, n);
	}
	return op->size;
}

static int set_reg_profile(RAnal *anal) {
	RStrBuf sb;
	const char *p = \
		"=PC	pc\n"
		"=SP	y\n"
		"=A0	r24\n"
		"=A1	r25\n"
		"=A2	r26\n"
		"=A3	r27\n"
		"gpr	psr	.32	0	0\n"
		"gpr	pc	.32	4	0\n"
		"gpr	npc	.32	8	0\n"
		"gpr	y	.32	12	0\n"
		/* r0-r7 are global aka g0-g7 */
		"gpr	r0	.32	16	0\n"
		"gpr	r1	.32	20	0\n"
		"gpr	r2	.32	24	0\n"
		"gpr	r3	.32	28	0\n"
		"gpr	r4	.32	32	0\n"
		"gpr	r5	.32	36	0\n"
		"gpr	r6	.32	40	0\n"
		"gpr	r7	.32	44	0\n"
		/* r8-15 are out (o0-o7) */
		"gpr	r8	.32	48	0\n"
		"gpr	r9	.32	52	0\n"
		"gpr	r10	.32	56	0\n"
		"gpr	r11	.32	60	0\n"
		"gpr	r12	.32	64	0\n"
		"gpr	r13	.32	68	0\n"
		"gpr	r14	.32	72	0\n"
		"gpr	r15	.32	76	0\n"
		/* r16-23 are local (o0-o7) */
		"gpr	r16	.32	80	0\n"
		"gpr	r17	.32	84	0\n"
		"gpr	r18	.32	88	0\n"
		"gpr	r19	.32	92	0\n"
		"gpr	r20	.32	96	0\n"
		"gpr	r21	.32	100	0\n"
		"gpr	r22	.32	104	0\n"
		"gpr	r23	.32	108	0\n"
		/* r24-31 are in (i0-i7) */
		"gpr	r24	.32	112	0\n"
		"gpr	r25	.32	116	0\n"
		"gpr	r26	.32	120	0\n"
		"gpr	r27	.32	124	0\n"
		"gpr	r28	.32	128	0\n"
		"gpr	r29	.32	132	0\n"
		"gpr	r30	.32	136	0\n"
		"gpr	r31	.32	140	0\n"
	;
	r_strbuf_init_const (&sb, p, strlen (p));
	return r_reg_set_profile_string (anal->reg, &sb);
}

static int archinfo(RAnal *anal, int q) {
	return 4; /* :D */
}

RAnalPlugin r_anal_plugin_sparc_cs = {
	.name = "sparc",
	.desc = "Capstone SPARC analysis",
	.esil = true,
	.license = "BSD",
	.arch = "sparc",
	.bits = 32|64,
	.archinfo = archinfo,
	.op = &analop,
	.set_reg_profile = &set_reg_profile,
};

#ifndef R2_PLUGIN_INCORE
R_API RLibStruct radare_plugin = {
	.type = R_LIB_TYPE_ANAL,
	.data = &r_anal_plugin_sparc_cs,
	.version = R2_VERSION
};
#endif
