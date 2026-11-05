// Written by tools/ppc_gen.py from ppc-instructions.xml (Xenia's, BSD). Do not edit.
// clang-format off

void disasm_addcx([[maybe_unused]] const Data &d, std::string &out) {
    // addc[OE][Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "addc";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_addex([[maybe_unused]] const Data &d, std::string &out) {
    // adde[OE][Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "adde";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_addi([[maybe_unused]] const Data &d, std::string &out) {
    // addi [RD], [RA0], [SIMM]
    size_t start = out.size();
    out += "addi";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    out += "r" + std::to_string(d.D.RA0());
    out += ", ";
    signed_hex(out, d.D.SIMM());
}
void disasm_addic([[maybe_unused]] const Data &d, std::string &out) {
    // addic [RD], [RA], [SIMM]
    size_t start = out.size();
    out += "addic";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    signed_hex(out, d.D.SIMM());
}
void disasm_addicx([[maybe_unused]] const Data &d, std::string &out) {
    // addic. [RD], [RA], [SIMM]
    size_t start = out.size();
    out += "addic.";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    signed_hex(out, d.D.SIMM());
}
void disasm_addis([[maybe_unused]] const Data &d, std::string &out) {
    // addis [RD], [RA0], [SIMM]
    size_t start = out.size();
    out += "addis";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    out += "r" + std::to_string(d.D.RA0());
    out += ", ";
    signed_hex(out, d.D.SIMM());
}
void disasm_addmex([[maybe_unused]] const Data &d, std::string &out) {
    // addme[OE][Rc] [RD], [RA]
    size_t start = out.size();
    out += "addme";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
}
void disasm_addx([[maybe_unused]] const Data &d, std::string &out) {
    // add[OE][Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "add";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_addzex([[maybe_unused]] const Data &d, std::string &out) {
    // addze[OE][Rc] [RD], [RA]
    size_t start = out.size();
    out += "addze";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
}
void disasm_andcx([[maybe_unused]] const Data &d, std::string &out) {
    // andc[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "andc";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_andisx([[maybe_unused]] const Data &d, std::string &out) {
    // andis. [RA], [RS], [UIMM]
    size_t start = out.size();
    out += "andis.";
    pad(out, start);
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    out += "r" + std::to_string(d.D.RS());
    out += ", ";
    hex(out, d.D.UIMM());
}
void disasm_andix([[maybe_unused]] const Data &d, std::string &out) {
    // andi. [RA], [RS], [UIMM]
    size_t start = out.size();
    out += "andi.";
    pad(out, start);
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    out += "r" + std::to_string(d.D.RS());
    out += ", ";
    hex(out, d.D.UIMM());
}
void disasm_andx([[maybe_unused]] const Data &d, std::string &out) {
    // and[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "and";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_bcctrx([[maybe_unused]] const Data &d, std::string &out) {
    // bcctr[LK] [BO], [BI]
    size_t start = out.size();
    out += "bcctr";
    if (d.XL.LK()) out += 'l';
    pad(out, start);
    dec(out, d.XL.BO());
    out += ", ";
    dec(out, d.XL.BI());
}
void disasm_bclrx([[maybe_unused]] const Data &d, std::string &out) {
    // bclr[LK] [BO], [BI]
    size_t start = out.size();
    out += "bclr";
    if (d.XL.LK()) out += 'l';
    pad(out, start);
    dec(out, d.XL.BO());
    out += ", ";
    dec(out, d.XL.BI());
}
void disasm_bx([[maybe_unused]] const Data &d, std::string &out) {
    // b[LK][AA] [ADDR]
    size_t start = out.size();
    out += "b";
    if (d.I.LK()) out += 'l';
    if (d.I.AA()) out += 'a';
    pad(out, start);
    hex(out, d.I.ADDR());
}
void disasm_cmp([[maybe_unused]] const Data &d, std::string &out) {
    // cmp [CRFD], [L], [RA], [RB]
    size_t start = out.size();
    out += "cmp";
    pad(out, start);
    out += "crf" + std::to_string(d.X.CRFD());
    out += ", ";
    dec(out, d.X.L());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_cmpi([[maybe_unused]] const Data &d, std::string &out) {
    // cmpi [CRFD], [L], [RA], [SIMM]
    size_t start = out.size();
    out += "cmpi";
    pad(out, start);
    out += "crf" + std::to_string(d.D.CRFD());
    out += ", ";
    dec(out, d.D.L());
    out += ", ";
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    signed_hex(out, d.D.SIMM());
}
void disasm_cmpl([[maybe_unused]] const Data &d, std::string &out) {
    // cmpl [CRFD], [L], [RA], [RB]
    size_t start = out.size();
    out += "cmpl";
    pad(out, start);
    out += "crf" + std::to_string(d.X.CRFD());
    out += ", ";
    dec(out, d.X.L());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_cmpli([[maybe_unused]] const Data &d, std::string &out) {
    // cmpli [CRFD], [L], [RA], [UIMM]
    size_t start = out.size();
    out += "cmpli";
    pad(out, start);
    out += "crf" + std::to_string(d.D.CRFD());
    out += ", ";
    dec(out, d.D.L());
    out += ", ";
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    hex(out, d.D.UIMM());
}
void disasm_cntlzdx([[maybe_unused]] const Data &d, std::string &out) {
    // cntlzd [RA], [RS]
    size_t start = out.size();
    out += "cntlzd";
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
}
void disasm_cntlzwx([[maybe_unused]] const Data &d, std::string &out) {
    // cntlzw[Rc] [RA], [RS]
    size_t start = out.size();
    out += "cntlzw";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
}
void disasm_crand([[maybe_unused]] const Data &d, std::string &out) {
    // crand [CRBD], [CRBA], [CRBB]
    size_t start = out.size();
    out += "crand";
    pad(out, start);
    out += "crb" + std::to_string(d.XL.CRBD());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBA());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBB());
}
void disasm_crandc([[maybe_unused]] const Data &d, std::string &out) {
    // crandc [CRBD], [CRBA], [CRBB]
    size_t start = out.size();
    out += "crandc";
    pad(out, start);
    out += "crb" + std::to_string(d.XL.CRBD());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBA());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBB());
}
void disasm_creqv([[maybe_unused]] const Data &d, std::string &out) {
    // creqv [CRBD], [CRBA], [CRBB]
    size_t start = out.size();
    out += "creqv";
    pad(out, start);
    out += "crb" + std::to_string(d.XL.CRBD());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBA());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBB());
}
void disasm_crnand([[maybe_unused]] const Data &d, std::string &out) {
    // crnand [CRBD], [CRBA], [CRBB]
    size_t start = out.size();
    out += "crnand";
    pad(out, start);
    out += "crb" + std::to_string(d.XL.CRBD());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBA());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBB());
}
void disasm_crnor([[maybe_unused]] const Data &d, std::string &out) {
    // crnor [CRBD], [CRBA], [CRBB]
    size_t start = out.size();
    out += "crnor";
    pad(out, start);
    out += "crb" + std::to_string(d.XL.CRBD());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBA());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBB());
}
void disasm_cror([[maybe_unused]] const Data &d, std::string &out) {
    // cror [CRBD], [CRBA], [CRBB]
    size_t start = out.size();
    out += "cror";
    pad(out, start);
    out += "crb" + std::to_string(d.XL.CRBD());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBA());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBB());
}
void disasm_crorc([[maybe_unused]] const Data &d, std::string &out) {
    // crorc [CRBD], [CRBA], [CRBB]
    size_t start = out.size();
    out += "crorc";
    pad(out, start);
    out += "crb" + std::to_string(d.XL.CRBD());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBA());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBB());
}
void disasm_crxor([[maybe_unused]] const Data &d, std::string &out) {
    // crxor [CRBD], [CRBA], [CRBB]
    size_t start = out.size();
    out += "crxor";
    pad(out, start);
    out += "crb" + std::to_string(d.XL.CRBD());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBA());
    out += ", ";
    out += "crb" + std::to_string(d.XL.CRBB());
}
void disasm_dcbf([[maybe_unused]] const Data &d, std::string &out) {
    // dcbf [RA0], [RB]
    size_t start = out.size();
    out += "dcbf";
    pad(out, start);
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_dcbi([[maybe_unused]] const Data &d, std::string &out) {
    // dcbi [RA0], [RB]
    size_t start = out.size();
    out += "dcbi";
    pad(out, start);
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_dcbst([[maybe_unused]] const Data &d, std::string &out) {
    // dcbst [RA0], [RB]
    size_t start = out.size();
    out += "dcbst";
    pad(out, start);
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_dcbt([[maybe_unused]] const Data &d, std::string &out) {
    // dcbt [RA0], [RB]
    size_t start = out.size();
    out += "dcbt";
    pad(out, start);
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_dcbtst([[maybe_unused]] const Data &d, std::string &out) {
    // dcbtst [RA0], [RB]
    size_t start = out.size();
    out += "dcbtst";
    pad(out, start);
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_dcbz([[maybe_unused]] const Data &d, std::string &out) {
    // dcbz [RA0], [RB]
    size_t start = out.size();
    out += "dcbz";
    pad(out, start);
    out += "r" + std::to_string(d.DCBZ.RA0());
    out += ", ";
    out += "r" + std::to_string(d.DCBZ.RB());
}
void disasm_dcbz128([[maybe_unused]] const Data &d, std::string &out) {
    // dcbz128 [RA0], [RB]
    size_t start = out.size();
    out += "dcbz128";
    pad(out, start);
    out += "r" + std::to_string(d.DCBZ.RA0());
    out += ", ";
    out += "r" + std::to_string(d.DCBZ.RB());
}
void disasm_divdux([[maybe_unused]] const Data &d, std::string &out) {
    // divdu[OE][Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "divdu";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_divdx([[maybe_unused]] const Data &d, std::string &out) {
    // divd[OE][Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "divd";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_divwux([[maybe_unused]] const Data &d, std::string &out) {
    // divwu[OE][Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "divwu";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_divwx([[maybe_unused]] const Data &d, std::string &out) {
    // divw[OE][Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "divw";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_eieio([[maybe_unused]] const Data &d, std::string &out) {
    // eieio
    size_t start = out.size();
    out += "eieio";
    pad(out, start);
}
void disasm_eqvx([[maybe_unused]] const Data &d, std::string &out) {
    // eqv[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "eqv";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_extsbx([[maybe_unused]] const Data &d, std::string &out) {
    // extsb[Rc] [RA], [RS]
    size_t start = out.size();
    out += "extsb";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
}
void disasm_extshx([[maybe_unused]] const Data &d, std::string &out) {
    // extsh[Rc] [RA], [RS]
    size_t start = out.size();
    out += "extsh";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
}
void disasm_extswx([[maybe_unused]] const Data &d, std::string &out) {
    // extsw[Rc] [RA], [RS]
    size_t start = out.size();
    out += "extsw";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
}
void disasm_fabsx([[maybe_unused]] const Data &d, std::string &out) {
    // fabs[Rc] [FD], [FB]
    size_t start = out.size();
    out += "fabs";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "fr" + std::to_string(d.X.FB());
}
void disasm_faddsx([[maybe_unused]] const Data &d, std::string &out) {
    // fadds[Rc] [FD], [FA], [FB]
    size_t start = out.size();
    out += "fadds";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_faddx([[maybe_unused]] const Data &d, std::string &out) {
    // fadd[Rc] [FD], [FA], [FB]
    size_t start = out.size();
    out += "fadd";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fcfidx([[maybe_unused]] const Data &d, std::string &out) {
    // fcfid[Rc] [FD], [FB]
    size_t start = out.size();
    out += "fcfid";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "fr" + std::to_string(d.X.FB());
}
void disasm_fcmpo([[maybe_unused]] const Data &d, std::string &out) {
    // fcmpo [CRFD], [FA], [FB]
    size_t start = out.size();
    out += "fcmpo";
    pad(out, start);
    out += "crf" + std::to_string(d.X.CRFD());
    out += ", ";
    out += "fr" + std::to_string(d.X.FA());
    out += ", ";
    out += "fr" + std::to_string(d.X.FB());
}
void disasm_fcmpu([[maybe_unused]] const Data &d, std::string &out) {
    // fcmpu [CRFD], [FA], [FB]
    size_t start = out.size();
    out += "fcmpu";
    pad(out, start);
    out += "crf" + std::to_string(d.X.CRFD());
    out += ", ";
    out += "fr" + std::to_string(d.X.FA());
    out += ", ";
    out += "fr" + std::to_string(d.X.FB());
}
void disasm_fctidx([[maybe_unused]] const Data &d, std::string &out) {
    // fctid[Rc] [FD], [FB]
    size_t start = out.size();
    out += "fctid";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "fr" + std::to_string(d.X.FB());
}
void disasm_fctidzx([[maybe_unused]] const Data &d, std::string &out) {
    // fctidz[Rc] [FD], [FB]
    size_t start = out.size();
    out += "fctidz";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "fr" + std::to_string(d.X.FB());
}
void disasm_fctiwx([[maybe_unused]] const Data &d, std::string &out) {
    // fctiw[Rc] [FD], [FB]
    size_t start = out.size();
    out += "fctiw";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "fr" + std::to_string(d.X.FB());
}
void disasm_fctiwzx([[maybe_unused]] const Data &d, std::string &out) {
    // fctiwz[Rc] [FD], [FB]
    size_t start = out.size();
    out += "fctiwz";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "fr" + std::to_string(d.X.FB());
}
void disasm_fdivsx([[maybe_unused]] const Data &d, std::string &out) {
    // fdivs[Rc] [FD], [FA], [FB]
    size_t start = out.size();
    out += "fdivs";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fdivx([[maybe_unused]] const Data &d, std::string &out) {
    // fdiv[Rc] [FD], [FA], [FB]
    size_t start = out.size();
    out += "fdiv";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fmaddsx([[maybe_unused]] const Data &d, std::string &out) {
    // fmadds[Rc] [FD], [FA], [FC], [FB]
    size_t start = out.size();
    out += "fmadds";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FC());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fmaddx([[maybe_unused]] const Data &d, std::string &out) {
    // fmadd[Rc] [FD], [FA], [FC], [FB]
    size_t start = out.size();
    out += "fmadd";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FC());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fmrx([[maybe_unused]] const Data &d, std::string &out) {
    // fmr[Rc] [FD], [FB]
    size_t start = out.size();
    out += "fmr";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "fr" + std::to_string(d.X.FB());
}
void disasm_fmsubsx([[maybe_unused]] const Data &d, std::string &out) {
    // fmsubs[Rc] [FD], [FA], [FC], [FB]
    size_t start = out.size();
    out += "fmsubs";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FC());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fmsubx([[maybe_unused]] const Data &d, std::string &out) {
    // fmsub[Rc] [FD], [FA], [FC], [FB]
    size_t start = out.size();
    out += "fmsub";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FC());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fmulsx([[maybe_unused]] const Data &d, std::string &out) {
    // fmuls[Rc] [FD], [FA], [FC]
    size_t start = out.size();
    out += "fmuls";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FC());
}
void disasm_fmulx([[maybe_unused]] const Data &d, std::string &out) {
    // fmul[Rc] [FD], [FA], [FC]
    size_t start = out.size();
    out += "fmul";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FC());
}
void disasm_fnabsx([[maybe_unused]] const Data &d, std::string &out) {
    // fnabs[Rc] [FD], [FB]
    size_t start = out.size();
    out += "fnabs";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "fr" + std::to_string(d.X.FB());
}
void disasm_fnegx([[maybe_unused]] const Data &d, std::string &out) {
    // fneg[Rc] [FD], [FB]
    size_t start = out.size();
    out += "fneg";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "fr" + std::to_string(d.X.FB());
}
void disasm_fnmaddsx([[maybe_unused]] const Data &d, std::string &out) {
    // fnmadds[Rc] [FD], [FA], [FC], [FB]
    size_t start = out.size();
    out += "fnmadds";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FC());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fnmaddx([[maybe_unused]] const Data &d, std::string &out) {
    // fnmadd[Rc] [FD], [FA], [FC], [FB]
    size_t start = out.size();
    out += "fnmadd";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FC());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fnmsubsx([[maybe_unused]] const Data &d, std::string &out) {
    // fnmsubs[Rc] [FD], [FA], [FC], [FB]
    size_t start = out.size();
    out += "fnmsubs";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FC());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fnmsubx([[maybe_unused]] const Data &d, std::string &out) {
    // fnmsub[Rc] [FD], [FA], [FC], [FB]
    size_t start = out.size();
    out += "fnmsub";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FC());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fresx([[maybe_unused]] const Data &d, std::string &out) {
    // fres[Rc] [FD], [FB]
    size_t start = out.size();
    out += "fres";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_frspx([[maybe_unused]] const Data &d, std::string &out) {
    // frsp[Rc] [FD], [FB]
    size_t start = out.size();
    out += "frsp";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "fr" + std::to_string(d.X.FB());
}
void disasm_frsqrtex([[maybe_unused]] const Data &d, std::string &out) {
    // frsqrte[Rc] [FD], [FB]
    size_t start = out.size();
    out += "frsqrte";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fselx([[maybe_unused]] const Data &d, std::string &out) {
    // fsel[Rc] [FD], [FA], [FC], [FB]
    size_t start = out.size();
    out += "fsel";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FC());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fsqrtsx([[maybe_unused]] const Data &d, std::string &out) {
    // fsqrts[Rc] [FD], [FB]
    size_t start = out.size();
    out += "fsqrts";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fsqrtx([[maybe_unused]] const Data &d, std::string &out) {
    // fsqrt[Rc] [FD], [FB]
    size_t start = out.size();
    out += "fsqrt";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fsubsx([[maybe_unused]] const Data &d, std::string &out) {
    // fsubs[Rc] [FD], [FA], [FB]
    size_t start = out.size();
    out += "fsubs";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_fsubx([[maybe_unused]] const Data &d, std::string &out) {
    // fsub[Rc] [FD], [FA], [FB]
    size_t start = out.size();
    out += "fsub";
    if (d.A.Rc()) out += '.';
    pad(out, start);
    out += "fr" + std::to_string(d.A.FD());
    out += ", ";
    out += "fr" + std::to_string(d.A.FA());
    out += ", ";
    out += "fr" + std::to_string(d.A.FB());
}
void disasm_icbi([[maybe_unused]] const Data &d, std::string &out) {
    // icbi [RA], [RB]
    size_t start = out.size();
    out += "icbi";
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_isync([[maybe_unused]] const Data &d, std::string &out) {
    // isync
    size_t start = out.size();
    out += "isync";
    pad(out, start);
}
void disasm_lbz([[maybe_unused]] const Data &d, std::string &out) {
    // lbz [RD], [d]([RA0])
    size_t start = out.size();
    out += "lbz";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA0());
    out += ")";
}
void disasm_lbzu([[maybe_unused]] const Data &d, std::string &out) {
    // lbzu [RD], [d]([RA])
    size_t start = out.size();
    out += "lbzu";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA());
    out += ")";
}
void disasm_lbzux([[maybe_unused]] const Data &d, std::string &out) {
    // lbzux [RD], [RA], [RB]
    size_t start = out.size();
    out += "lbzux";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lbzx([[maybe_unused]] const Data &d, std::string &out) {
    // lbzx [RD], [RA0], [RB]
    size_t start = out.size();
    out += "lbzx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_ld([[maybe_unused]] const Data &d, std::string &out) {
    // ld [RD], [ds]([RA0])
    size_t start = out.size();
    out += "ld";
    pad(out, start);
    out += "r" + std::to_string(d.DS.RD());
    out += ", ";
    signed_hex(out, d.DS.ds());
    out += "(";
    out += "r" + std::to_string(d.DS.RA0());
    out += ")";
}
void disasm_ldarx([[maybe_unused]] const Data &d, std::string &out) {
    // ldarx [RD], [RA0], [RB]
    size_t start = out.size();
    out += "ldarx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_ldbrx([[maybe_unused]] const Data &d, std::string &out) {
    // ldbrx [RD], [RA0], [RB]
    size_t start = out.size();
    out += "ldbrx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_ldu([[maybe_unused]] const Data &d, std::string &out) {
    // ldu [RD], [ds]([RA])
    size_t start = out.size();
    out += "ldu";
    pad(out, start);
    out += "r" + std::to_string(d.DS.RD());
    out += ", ";
    signed_hex(out, d.DS.ds());
    out += "(";
    out += "r" + std::to_string(d.DS.RA());
    out += ")";
}
void disasm_ldux([[maybe_unused]] const Data &d, std::string &out) {
    // ldux [RD], [RA], [RB]
    size_t start = out.size();
    out += "ldux";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_ldx([[maybe_unused]] const Data &d, std::string &out) {
    // ldx [RD], [RA0], [RB]
    size_t start = out.size();
    out += "ldx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lfd([[maybe_unused]] const Data &d, std::string &out) {
    // lfd [FD], [d]([RA0])
    size_t start = out.size();
    out += "lfd";
    pad(out, start);
    out += "fr" + std::to_string(d.D.FD());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA0());
    out += ")";
}
void disasm_lfdu([[maybe_unused]] const Data &d, std::string &out) {
    // lfdu [FD], [d]([RA])
    size_t start = out.size();
    out += "lfdu";
    pad(out, start);
    out += "fr" + std::to_string(d.D.FD());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA());
    out += ")";
}
void disasm_lfdux([[maybe_unused]] const Data &d, std::string &out) {
    // lfdux [FD], [RA], [RB]
    size_t start = out.size();
    out += "lfdux";
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lfdx([[maybe_unused]] const Data &d, std::string &out) {
    // lfdx [FD], [RA0], [RB]
    size_t start = out.size();
    out += "lfdx";
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lfs([[maybe_unused]] const Data &d, std::string &out) {
    // lfs [FD], [d]([RA0])
    size_t start = out.size();
    out += "lfs";
    pad(out, start);
    out += "fr" + std::to_string(d.D.FD());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA0());
    out += ")";
}
void disasm_lfsu([[maybe_unused]] const Data &d, std::string &out) {
    // lfsu [FD], [d]([RA])
    size_t start = out.size();
    out += "lfsu";
    pad(out, start);
    out += "fr" + std::to_string(d.D.FD());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA());
    out += ")";
}
void disasm_lfsux([[maybe_unused]] const Data &d, std::string &out) {
    // lfsux [FD], [RA], [RB]
    size_t start = out.size();
    out += "lfsux";
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lfsx([[maybe_unused]] const Data &d, std::string &out) {
    // lfsx [FD], [RA0], [RB]
    size_t start = out.size();
    out += "lfsx";
    pad(out, start);
    out += "fr" + std::to_string(d.X.FD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lha([[maybe_unused]] const Data &d, std::string &out) {
    // lha [RD], [d]([RA0])
    size_t start = out.size();
    out += "lha";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA0());
    out += ")";
}
void disasm_lhau([[maybe_unused]] const Data &d, std::string &out) {
    // lhau [RD], [d]([RA])
    size_t start = out.size();
    out += "lhau";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA());
    out += ")";
}
void disasm_lhaux([[maybe_unused]] const Data &d, std::string &out) {
    // lhaux [RD], [RA], [RB]
    size_t start = out.size();
    out += "lhaux";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lhax([[maybe_unused]] const Data &d, std::string &out) {
    // lhax [RD], [RA0], [RB]
    size_t start = out.size();
    out += "lhax";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lhbrx([[maybe_unused]] const Data &d, std::string &out) {
    // lhbrx [RD], [RA0], [RB]
    size_t start = out.size();
    out += "lhbrx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lhz([[maybe_unused]] const Data &d, std::string &out) {
    // lhz [RD], [d]([RA0])
    size_t start = out.size();
    out += "lhz";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA0());
    out += ")";
}
void disasm_lhzu([[maybe_unused]] const Data &d, std::string &out) {
    // lhzu [RD], [d]([RA])
    size_t start = out.size();
    out += "lhzu";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA());
    out += ")";
}
void disasm_lhzux([[maybe_unused]] const Data &d, std::string &out) {
    // lhzux [RD], [RA], [RB]
    size_t start = out.size();
    out += "lhzux";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lhzx([[maybe_unused]] const Data &d, std::string &out) {
    // lhzx [RD], [RA0], [RB]
    size_t start = out.size();
    out += "lhzx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lvebx([[maybe_unused]] const Data &d, std::string &out) {
    // lvebx [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvebx";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lvehx([[maybe_unused]] const Data &d, std::string &out) {
    // lvehx [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvehx";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lvewx([[maybe_unused]] const Data &d, std::string &out) {
    // lvewx [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvewx";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lvewx128([[maybe_unused]] const Data &d, std::string &out) {
    // lvewx128 [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvewx128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VD());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_lvlx([[maybe_unused]] const Data &d, std::string &out) {
    // lvlx [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvlx";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lvlx128([[maybe_unused]] const Data &d, std::string &out) {
    // lvlx128 [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvlx128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VD());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_lvlxl([[maybe_unused]] const Data &d, std::string &out) {
    // lvlxl [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvlxl";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lvlxl128([[maybe_unused]] const Data &d, std::string &out) {
    // lvlxl128 [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvlxl128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VD());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_lvrx([[maybe_unused]] const Data &d, std::string &out) {
    // lvrx [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvrx";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lvrx128([[maybe_unused]] const Data &d, std::string &out) {
    // lvrx128 [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvrx128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VD());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_lvrxl([[maybe_unused]] const Data &d, std::string &out) {
    // lvrxl [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvrxl";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lvrxl128([[maybe_unused]] const Data &d, std::string &out) {
    // lvrxl128 [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvrxl128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VD());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_lvsl([[maybe_unused]] const Data &d, std::string &out) {
    // lvsl [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvsl";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lvsl128([[maybe_unused]] const Data &d, std::string &out) {
    // lvsl128 [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvsl128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VD());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_lvsr([[maybe_unused]] const Data &d, std::string &out) {
    // lvsr [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvsr";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lvsr128([[maybe_unused]] const Data &d, std::string &out) {
    // lvsr128 [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvsr128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VD());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_lvx([[maybe_unused]] const Data &d, std::string &out) {
    // lvx [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvx";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lvx128([[maybe_unused]] const Data &d, std::string &out) {
    // lvx128 [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvx128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VD());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_lvxl([[maybe_unused]] const Data &d, std::string &out) {
    // lvslx [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvslx";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lvxl128([[maybe_unused]] const Data &d, std::string &out) {
    // lvxl128 [VD], [RA0], [RB]
    size_t start = out.size();
    out += "lvxl128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VD());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_lwa([[maybe_unused]] const Data &d, std::string &out) {
    // lwa [RD], [ds]([RA0])
    size_t start = out.size();
    out += "lwa";
    pad(out, start);
    out += "r" + std::to_string(d.DS.RD());
    out += ", ";
    signed_hex(out, d.DS.ds());
    out += "(";
    out += "r" + std::to_string(d.DS.RA0());
    out += ")";
}
void disasm_lwarx([[maybe_unused]] const Data &d, std::string &out) {
    // lwarx [RD], [RA0], [RB]
    size_t start = out.size();
    out += "lwarx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lwaux([[maybe_unused]] const Data &d, std::string &out) {
    // lwaux [RD], [RA], [RB]
    size_t start = out.size();
    out += "lwaux";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lwax([[maybe_unused]] const Data &d, std::string &out) {
    // lwax [RD], [RA0], [RB]
    size_t start = out.size();
    out += "lwax";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lwbrx([[maybe_unused]] const Data &d, std::string &out) {
    // lwbrx [RD], [RA0], [RB]
    size_t start = out.size();
    out += "lwbrx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lwz([[maybe_unused]] const Data &d, std::string &out) {
    // lwz [RD], [d]([RA0])
    size_t start = out.size();
    out += "lwz";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA0());
    out += ")";
}
void disasm_lwzu([[maybe_unused]] const Data &d, std::string &out) {
    // lwzu [RD], [d]([RA])
    size_t start = out.size();
    out += "lwzu";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA());
    out += ")";
}
void disasm_lwzux([[maybe_unused]] const Data &d, std::string &out) {
    // lwzux [RD], [RA], [RB]
    size_t start = out.size();
    out += "lwzux";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_lwzx([[maybe_unused]] const Data &d, std::string &out) {
    // lwzx [RD], [RA0], [RB]
    size_t start = out.size();
    out += "lwzx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_mcrf([[maybe_unused]] const Data &d, std::string &out) {
    // mcrf [CRFD], [CRFS]
    size_t start = out.size();
    out += "mcrf";
    pad(out, start);
    out += "crf" + std::to_string(d.XL.CRFD());
    out += ", ";
    out += "crf" + std::to_string(d.XL.CRFS());
}
void disasm_mcrfs([[maybe_unused]] const Data &d, std::string &out) {
    // mcrfs [CRFD], [CRFS]
    size_t start = out.size();
    out += "mcrfs";
    pad(out, start);
    out += "crf" + std::to_string(d.X.CRFD());
    out += ", ";
    out += "crf" + std::to_string(d.X.CRFS());
}
void disasm_mcrxr([[maybe_unused]] const Data &d, std::string &out) {
    // mcrxr [CRFD]
    size_t start = out.size();
    out += "mcrxr";
    pad(out, start);
    out += "crf" + std::to_string(d.X.CRFD());
}
void disasm_mfcr([[maybe_unused]] const Data &d, std::string &out) {
    // mfcr [RD]
    size_t start = out.size();
    out += "mfcr";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
}
void disasm_mffsx([[maybe_unused]] const Data &d, std::string &out) {
    // mffs[Rc] [RD]
    size_t start = out.size();
    out += "mffs";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
}
void disasm_mfmsr([[maybe_unused]] const Data &d, std::string &out) {
    // mfmsr [RD]
    size_t start = out.size();
    out += "mfmsr";
    pad(out, start);
    out += "r" + std::to_string(d.X.RD());
}
void disasm_mfspr([[maybe_unused]] const Data &d, std::string &out) {
    // mfspr [RD], [SPR]
    size_t start = out.size();
    out += "mfspr";
    pad(out, start);
    out += "r" + std::to_string(d.XFX.RD());
    out += ", ";
    dec(out, d.XFX.SPR());
}
void disasm_mftb([[maybe_unused]] const Data &d, std::string &out) {
    // mftb [RD], [TBR]
    size_t start = out.size();
    out += "mftb";
    pad(out, start);
    out += "r" + std::to_string(d.XFX.RD());
    out += ", ";
    dec(out, d.XFX.TBR());
}
void disasm_mtcrf([[maybe_unused]] const Data &d, std::string &out) {
    // mtcrf [CRM], [RS]
    size_t start = out.size();
    out += "mtcrf";
    pad(out, start);
    out += "(UNHANDLED CRM)";
    out += ", ";
    out += "r" + std::to_string(d.XFX.RS());
}
void disasm_mtfsb0x([[maybe_unused]] const Data &d, std::string &out) {
    // mtfsb0[Rc] [FPSCRD]
    size_t start = out.size();
    out += "mtfsb0";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "(UNHANDLED FPSCRD)";
}
void disasm_mtfsb1x([[maybe_unused]] const Data &d, std::string &out) {
    // mtfsb1[Rc] [FPSCRD]
    size_t start = out.size();
    out += "mtfsb1";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "(UNHANDLED FPSCRD)";
}
void disasm_mtfsfix([[maybe_unused]] const Data &d, std::string &out) {
    // mtfsfi[Rc] [CRFD], [IMM]
    size_t start = out.size();
    out += "mtfsfi";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "crf" + std::to_string(d.X.CRFD());
    out += ", ";
    dec(out, d.X.IMM());
}
void disasm_mtfsfx([[maybe_unused]] const Data &d, std::string &out) {
    // mtfsf[Rc] [FM], [FB]
    size_t start = out.size();
    out += "mtfsf";
    if (d.XFL.Rc()) out += '.';
    pad(out, start);
    dec(out, d.XFL.FM());
    out += ", ";
    out += "fr" + std::to_string(d.XFL.FB());
}
void disasm_mtmsr([[maybe_unused]] const Data &d, std::string &out) {
    // mtmsr [RS]
    size_t start = out.size();
    out += "mtmsr";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
}
void disasm_mtmsrd([[maybe_unused]] const Data &d, std::string &out) {
    // mtmsrd [RS]
    size_t start = out.size();
    out += "mtmsrd";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
}
void disasm_mtspr([[maybe_unused]] const Data &d, std::string &out) {
    // mtspr [SPR], [RS]
    size_t start = out.size();
    out += "mtspr";
    pad(out, start);
    dec(out, d.XFX.SPR());
    out += ", ";
    out += "r" + std::to_string(d.XFX.RS());
}
void disasm_mulhdux([[maybe_unused]] const Data &d, std::string &out) {
    // mulhdu[Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "mulhdu";
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_mulhdx([[maybe_unused]] const Data &d, std::string &out) {
    // mulhd[Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "mulhd";
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_mulhwux([[maybe_unused]] const Data &d, std::string &out) {
    // mulhwu[Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "mulhwu";
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_mulhwx([[maybe_unused]] const Data &d, std::string &out) {
    // mulhw[Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "mulhw";
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_mulldx([[maybe_unused]] const Data &d, std::string &out) {
    // mulld[OE][Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "mulld";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_mulli([[maybe_unused]] const Data &d, std::string &out) {
    // mulli [RD], [RA], [SIMM]
    size_t start = out.size();
    out += "mulli";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    signed_hex(out, d.D.SIMM());
}
void disasm_mullwx([[maybe_unused]] const Data &d, std::string &out) {
    // mullw[OE][Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "mullw";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_nandx([[maybe_unused]] const Data &d, std::string &out) {
    // nand[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "nand";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_negx([[maybe_unused]] const Data &d, std::string &out) {
    // neg[OE][Rc] [RD], [RA]
    size_t start = out.size();
    out += "neg";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
}
void disasm_norx([[maybe_unused]] const Data &d, std::string &out) {
    // nor[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "nor";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_orcx([[maybe_unused]] const Data &d, std::string &out) {
    // orc[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "orc";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_ori([[maybe_unused]] const Data &d, std::string &out) {
    // ori [RA], [RS], [UIMM]
    size_t start = out.size();
    out += "ori";
    pad(out, start);
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    out += "r" + std::to_string(d.D.RS());
    out += ", ";
    hex(out, d.D.UIMM());
}
void disasm_oris([[maybe_unused]] const Data &d, std::string &out) {
    // oris [RA], [RS], [UIMM]
    size_t start = out.size();
    out += "oris";
    pad(out, start);
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    out += "r" + std::to_string(d.D.RS());
    out += ", ";
    hex(out, d.D.UIMM());
}
void disasm_orx([[maybe_unused]] const Data &d, std::string &out) {
    // or[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "or";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_rldclx([[maybe_unused]] const Data &d, std::string &out) {
    // rldcl[Rc] [RA], [RS], [RB], [MB]
    size_t start = out.size();
    out += "rldcl";
    if (d.MDS.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.MDS.RA());
    out += ", ";
    out += "r" + std::to_string(d.MDS.RS());
    out += ", ";
    out += "r" + std::to_string(d.MDS.RB());
    out += ", ";
    dec(out, d.MDS.MB());
}
void disasm_rldcrx([[maybe_unused]] const Data &d, std::string &out) {
    // rldcr[Rc] [RA], [RS], [RB], [ME]
    size_t start = out.size();
    out += "rldcr";
    if (d.MDS.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.MDS.RA());
    out += ", ";
    out += "r" + std::to_string(d.MDS.RS());
    out += ", ";
    out += "r" + std::to_string(d.MDS.RB());
    out += ", ";
    dec(out, d.MDS.ME());
}
void disasm_rldiclx([[maybe_unused]] const Data &d, std::string &out) {
    // rldicl[Rc] [RA], [RS], [SH], [MB]
    size_t start = out.size();
    out += "rldicl";
    if (d.MD.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.MD.RA());
    out += ", ";
    out += "r" + std::to_string(d.MD.RS());
    out += ", ";
    dec(out, d.MD.SH());
    out += ", ";
    dec(out, d.MD.MB());
}
void disasm_rldicrx([[maybe_unused]] const Data &d, std::string &out) {
    // rldicr[Rc] [RA], [RS], [SH], [ME]
    size_t start = out.size();
    out += "rldicr";
    if (d.MD.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.MD.RA());
    out += ", ";
    out += "r" + std::to_string(d.MD.RS());
    out += ", ";
    dec(out, d.MD.SH());
    out += ", ";
    dec(out, d.MD.ME());
}
void disasm_rldicx([[maybe_unused]] const Data &d, std::string &out) {
    // rldic[Rc] [RA], [RS], [SH], [MB]
    size_t start = out.size();
    out += "rldic";
    if (d.MD.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.MD.RA());
    out += ", ";
    out += "r" + std::to_string(d.MD.RS());
    out += ", ";
    dec(out, d.MD.SH());
    out += ", ";
    dec(out, d.MD.MB());
}
void disasm_rldimix([[maybe_unused]] const Data &d, std::string &out) {
    // rldimi[Rc] [RA], [RS], [SH], [MB]
    size_t start = out.size();
    out += "rldimi";
    if (d.MD.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.MD.RA());
    out += ", ";
    out += "r" + std::to_string(d.MD.RS());
    out += ", ";
    dec(out, d.MD.SH());
    out += ", ";
    dec(out, d.MD.MB());
}
void disasm_rlwimix([[maybe_unused]] const Data &d, std::string &out) {
    // rlwimi[Rc] [RA], [RS], [SH], [MB], [ME]
    size_t start = out.size();
    out += "rlwimi";
    if (d.M.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.M.RA());
    out += ", ";
    out += "r" + std::to_string(d.M.RS());
    out += ", ";
    dec(out, d.M.SH());
    out += ", ";
    dec(out, d.M.MB());
    out += ", ";
    dec(out, d.M.ME());
}
void disasm_rlwinmx([[maybe_unused]] const Data &d, std::string &out) {
    // rlwinm[Rc] [RA], [RS], [SH], [MB], [ME]
    size_t start = out.size();
    out += "rlwinm";
    if (d.M.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.M.RA());
    out += ", ";
    out += "r" + std::to_string(d.M.RS());
    out += ", ";
    dec(out, d.M.SH());
    out += ", ";
    dec(out, d.M.MB());
    out += ", ";
    dec(out, d.M.ME());
}
void disasm_rlwnmx([[maybe_unused]] const Data &d, std::string &out) {
    // rlwnm[Rc] [RA], [RS], [RB], [MB], [ME]
    size_t start = out.size();
    out += "rlwnm";
    if (d.M.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.M.RA());
    out += ", ";
    out += "r" + std::to_string(d.M.RS());
    out += ", ";
    out += "r" + std::to_string(d.M.RB());
    out += ", ";
    dec(out, d.M.MB());
    out += ", ";
    dec(out, d.M.ME());
}
void disasm_sc([[maybe_unused]] const Data &d, std::string &out) {
    // sc [LEV]
    size_t start = out.size();
    out += "sc";
    pad(out, start);
    if (d.SC.LEV()) dec(out, d.SC.LEV());
}
void disasm_sldx([[maybe_unused]] const Data &d, std::string &out) {
    // sld[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "sld";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_slwx([[maybe_unused]] const Data &d, std::string &out) {
    // slw[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "slw";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_sradix([[maybe_unused]] const Data &d, std::string &out) {
    // sradi[Rc] [RA], [RS], [SH]
    size_t start = out.size();
    out += "sradi";
    if (d.XS.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XS.RA());
    out += ", ";
    out += "r" + std::to_string(d.XS.RS());
    out += ", ";
    dec(out, d.XS.SH());
}
void disasm_sradx([[maybe_unused]] const Data &d, std::string &out) {
    // srad[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "srad";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_srawix([[maybe_unused]] const Data &d, std::string &out) {
    // srawi[Rc] [RA], [RS], [SH]
    size_t start = out.size();
    out += "srawi";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    dec(out, d.X.SH());
}
void disasm_srawx([[maybe_unused]] const Data &d, std::string &out) {
    // sraw[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "sraw";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_srdx([[maybe_unused]] const Data &d, std::string &out) {
    // srd[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "srd";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_srwx([[maybe_unused]] const Data &d, std::string &out) {
    // srw[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "srw";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stb([[maybe_unused]] const Data &d, std::string &out) {
    // stb [RS], [d]([RA0])
    size_t start = out.size();
    out += "stb";
    pad(out, start);
    out += "r" + std::to_string(d.D.RS());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA0());
    out += ")";
}
void disasm_stbu([[maybe_unused]] const Data &d, std::string &out) {
    // stbu [RS], [d]([RA])
    size_t start = out.size();
    out += "stbu";
    pad(out, start);
    out += "r" + std::to_string(d.D.RS());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA());
    out += ")";
}
void disasm_stbux([[maybe_unused]] const Data &d, std::string &out) {
    // stbux [RS], [RA], [RB]
    size_t start = out.size();
    out += "stbux";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stbx([[maybe_unused]] const Data &d, std::string &out) {
    // stbx [RS], [RA0], [RB]
    size_t start = out.size();
    out += "stbx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_std([[maybe_unused]] const Data &d, std::string &out) {
    // std [RS], [ds]([RA0])
    size_t start = out.size();
    out += "std";
    pad(out, start);
    out += "r" + std::to_string(d.DS.RS());
    out += ", ";
    signed_hex(out, d.DS.ds());
    out += "(";
    out += "r" + std::to_string(d.DS.RA0());
    out += ")";
}
void disasm_stdbrx([[maybe_unused]] const Data &d, std::string &out) {
    // stdbrx [RS], [RA0], [RB]
    size_t start = out.size();
    out += "stdbrx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stdcx([[maybe_unused]] const Data &d, std::string &out) {
    // stdcx. [RS], [RA0], [RB]
    size_t start = out.size();
    out += "stdcx.";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stdu([[maybe_unused]] const Data &d, std::string &out) {
    // stdu [RS], [ds]([RA])
    size_t start = out.size();
    out += "stdu";
    pad(out, start);
    out += "r" + std::to_string(d.DS.RS());
    out += ", ";
    signed_hex(out, d.DS.ds());
    out += "(";
    out += "r" + std::to_string(d.DS.RA());
    out += ")";
}
void disasm_stdux([[maybe_unused]] const Data &d, std::string &out) {
    // stdux [RS], [RA], [RB]
    size_t start = out.size();
    out += "stdux";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stdx([[maybe_unused]] const Data &d, std::string &out) {
    // stdx [RS], [RA0], [RB]
    size_t start = out.size();
    out += "stdx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stfd([[maybe_unused]] const Data &d, std::string &out) {
    // stfd [FS], [d]([RA0])
    size_t start = out.size();
    out += "stfd";
    pad(out, start);
    out += "fr" + std::to_string(d.D.FS());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA0());
    out += ")";
}
void disasm_stfdu([[maybe_unused]] const Data &d, std::string &out) {
    // stfdu [FS], [d]([RA])
    size_t start = out.size();
    out += "stfdu";
    pad(out, start);
    out += "fr" + std::to_string(d.D.FS());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA());
    out += ")";
}
void disasm_stfdux([[maybe_unused]] const Data &d, std::string &out) {
    // stfdux [FS], [RA], [RB]
    size_t start = out.size();
    out += "stfdux";
    pad(out, start);
    out += "fr" + std::to_string(d.X.FS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stfdx([[maybe_unused]] const Data &d, std::string &out) {
    // stfdx [FS], [RA0], [RB]
    size_t start = out.size();
    out += "stfdx";
    pad(out, start);
    out += "fr" + std::to_string(d.X.FS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stfiwx([[maybe_unused]] const Data &d, std::string &out) {
    // stfiwx [FS], [RA0], [RB]
    size_t start = out.size();
    out += "stfiwx";
    pad(out, start);
    out += "fr" + std::to_string(d.X.FS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stfs([[maybe_unused]] const Data &d, std::string &out) {
    // stfs [FS], [d]([RA0])
    size_t start = out.size();
    out += "stfs";
    pad(out, start);
    out += "fr" + std::to_string(d.D.FS());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA0());
    out += ")";
}
void disasm_stfsu([[maybe_unused]] const Data &d, std::string &out) {
    // stfsu [FS], [d]([RA])
    size_t start = out.size();
    out += "stfsu";
    pad(out, start);
    out += "fr" + std::to_string(d.D.FS());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA());
    out += ")";
}
void disasm_stfsux([[maybe_unused]] const Data &d, std::string &out) {
    // stfsux [FS], [RA], [RB]
    size_t start = out.size();
    out += "stfsux";
    pad(out, start);
    out += "fr" + std::to_string(d.X.FS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stfsx([[maybe_unused]] const Data &d, std::string &out) {
    // stfsx [FS], [RA], [RB]
    size_t start = out.size();
    out += "stfsx";
    pad(out, start);
    out += "fr" + std::to_string(d.X.FS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_sth([[maybe_unused]] const Data &d, std::string &out) {
    // sth [RS], [d]([RA0])
    size_t start = out.size();
    out += "sth";
    pad(out, start);
    out += "r" + std::to_string(d.D.RS());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA0());
    out += ")";
}
void disasm_sthbrx([[maybe_unused]] const Data &d, std::string &out) {
    // sthbrx [RS], [RA0], [RB]
    size_t start = out.size();
    out += "sthbrx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_sthu([[maybe_unused]] const Data &d, std::string &out) {
    // sthu [RS], [d]([RA])
    size_t start = out.size();
    out += "sthu";
    pad(out, start);
    out += "r" + std::to_string(d.D.RS());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA());
    out += ")";
}
void disasm_sthux([[maybe_unused]] const Data &d, std::string &out) {
    // sthux [RS], [RA], [RB]
    size_t start = out.size();
    out += "sthux";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_sthx([[maybe_unused]] const Data &d, std::string &out) {
    // sthx [RS], [RA0], [RB]
    size_t start = out.size();
    out += "sthx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stvebx([[maybe_unused]] const Data &d, std::string &out) {
    // stvebx [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvebx";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stvehx([[maybe_unused]] const Data &d, std::string &out) {
    // stvehx [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvehx";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stvewx([[maybe_unused]] const Data &d, std::string &out) {
    // stvewx [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvewx";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stvewx128([[maybe_unused]] const Data &d, std::string &out) {
    // stvewx128 [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvewx128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VS());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_stvlx([[maybe_unused]] const Data &d, std::string &out) {
    // stvlx [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvlx";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stvlx128([[maybe_unused]] const Data &d, std::string &out) {
    // stvlx128 [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvlx128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VS());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_stvlxl([[maybe_unused]] const Data &d, std::string &out) {
    // stvlxl [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvlxl";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stvlxl128([[maybe_unused]] const Data &d, std::string &out) {
    // stvlxl128 [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvlxl128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VS());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_stvrx([[maybe_unused]] const Data &d, std::string &out) {
    // stvrx [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvrx";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stvrx128([[maybe_unused]] const Data &d, std::string &out) {
    // stvrx128 [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvrx128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VS());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_stvrxl([[maybe_unused]] const Data &d, std::string &out) {
    // stvrxl [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvrxl";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stvrxl128([[maybe_unused]] const Data &d, std::string &out) {
    // stvrxl128 [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvrxl128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VS());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_stvx([[maybe_unused]] const Data &d, std::string &out) {
    // stvx [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvx";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stvx128([[maybe_unused]] const Data &d, std::string &out) {
    // stvx128 [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvx128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VS());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_stvxl([[maybe_unused]] const Data &d, std::string &out) {
    // stvxl [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvxl";
    pad(out, start);
    out += "vr" + std::to_string(d.X.VS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stvxl128([[maybe_unused]] const Data &d, std::string &out) {
    // stvxl128 [VS], [RA0], [RB]
    size_t start = out.size();
    out += "stvxl128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_1.VS());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RA0());
    out += ", ";
    out += "r" + std::to_string(d.VX128_1.RB());
}
void disasm_stw([[maybe_unused]] const Data &d, std::string &out) {
    // stw [RS], [d]([RA0])
    size_t start = out.size();
    out += "stw";
    pad(out, start);
    out += "r" + std::to_string(d.D.RS());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA0());
    out += ")";
}
void disasm_stwbrx([[maybe_unused]] const Data &d, std::string &out) {
    // stwbrx [RS], [RA0], [RB]
    size_t start = out.size();
    out += "stwbrx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stwcx([[maybe_unused]] const Data &d, std::string &out) {
    // stwcx. [RS], [RA0], [RB]
    size_t start = out.size();
    out += "stwcx.";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stwu([[maybe_unused]] const Data &d, std::string &out) {
    // stwu [RS], [d]([RA])
    size_t start = out.size();
    out += "stwu";
    pad(out, start);
    out += "r" + std::to_string(d.D.RS());
    out += ", ";
    signed_hex(out, d.D.d());
    out += "(";
    out += "r" + std::to_string(d.D.RA());
    out += ")";
}
void disasm_stwux([[maybe_unused]] const Data &d, std::string &out) {
    // stwux [RS], [RA], [RB]
    size_t start = out.size();
    out += "stwux";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_stwx([[maybe_unused]] const Data &d, std::string &out) {
    // stwx [RS], [RA0], [RB]
    size_t start = out.size();
    out += "stwx";
    pad(out, start);
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RA0());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_subfcx([[maybe_unused]] const Data &d, std::string &out) {
    // subfc[OE][Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "subfc";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_subfex([[maybe_unused]] const Data &d, std::string &out) {
    // subfe[OE][Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "subfe";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_subficx([[maybe_unused]] const Data &d, std::string &out) {
    // subfic [RD], [RA], [SIMM]
    size_t start = out.size();
    out += "subfic";
    pad(out, start);
    out += "r" + std::to_string(d.D.RD());
    out += ", ";
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    signed_hex(out, d.D.SIMM());
}
void disasm_subfmex([[maybe_unused]] const Data &d, std::string &out) {
    // subfme[OE][Rc] [RD], [RA]
    size_t start = out.size();
    out += "subfme";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
}
void disasm_subfx([[maybe_unused]] const Data &d, std::string &out) {
    // subf[OE][Rc] [RD], [RA], [RB]
    size_t start = out.size();
    out += "subf";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
    out += ", ";
    out += "r" + std::to_string(d.XO.RB());
}
void disasm_subfzex([[maybe_unused]] const Data &d, std::string &out) {
    // subfze[OE][Rc] [RD], [RA]
    size_t start = out.size();
    out += "subfze";
    if (d.XO.OE()) out += 'o';
    if (d.XO.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.XO.RD());
    out += ", ";
    out += "r" + std::to_string(d.XO.RA());
}
void disasm_sync([[maybe_unused]] const Data &d, std::string &out) {
    // sync
    size_t start = out.size();
    out += "sync";
    pad(out, start);
}
void disasm_td([[maybe_unused]] const Data &d, std::string &out) {
    // td [TO], [RA], [RB]
    size_t start = out.size();
    out += "td";
    pad(out, start);
    dec(out, d.X.TO());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_tdi([[maybe_unused]] const Data &d, std::string &out) {
    // tdi [TO], [RA], [SIMM]
    size_t start = out.size();
    out += "tdi";
    pad(out, start);
    dec(out, d.D.TO());
    out += ", ";
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    signed_hex(out, d.D.SIMM());
}
void disasm_tw([[maybe_unused]] const Data &d, std::string &out) {
    // tw [TO], [RA], [RB]
    size_t start = out.size();
    out += "tw";
    pad(out, start);
    dec(out, d.X.TO());
    out += ", ";
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}
void disasm_twi([[maybe_unused]] const Data &d, std::string &out) {
    // tw [TO], [RA], [SIMM]
    size_t start = out.size();
    out += "tw";
    pad(out, start);
    dec(out, d.D.TO());
    out += ", ";
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    signed_hex(out, d.D.SIMM());
}
void disasm_vaddcuw([[maybe_unused]] const Data &d, std::string &out) {
    // vaddcuw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vaddcuw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vaddfp([[maybe_unused]] const Data &d, std::string &out) {
    // vaddfp [VD], [VA], [VB]
    size_t start = out.size();
    out += "vaddfp";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vaddfp128([[maybe_unused]] const Data &d, std::string &out) {
    // vaddfp128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vaddfp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vaddsbs([[maybe_unused]] const Data &d, std::string &out) {
    // vaddsbs [VD], [VA], [VB]
    size_t start = out.size();
    out += "vaddsbs";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vaddshs([[maybe_unused]] const Data &d, std::string &out) {
    // vaddshs [VD], [VA], [VB]
    size_t start = out.size();
    out += "vaddshs";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vaddsws([[maybe_unused]] const Data &d, std::string &out) {
    // vaddsws [VD], [VA], [VB]
    size_t start = out.size();
    out += "vaddsws";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vaddubm([[maybe_unused]] const Data &d, std::string &out) {
    // vaddubm [VD], [VA], [VB]
    size_t start = out.size();
    out += "vaddubm";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vaddubs([[maybe_unused]] const Data &d, std::string &out) {
    // vaddubs [VD], [VA], [VB]
    size_t start = out.size();
    out += "vaddubs";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vadduhm([[maybe_unused]] const Data &d, std::string &out) {
    // vadduhm [VD], [VA], [VB]
    size_t start = out.size();
    out += "vadduhm";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vadduhs([[maybe_unused]] const Data &d, std::string &out) {
    // vadduhs [VD], [VA], [VB]
    size_t start = out.size();
    out += "vadduhs";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vadduwm([[maybe_unused]] const Data &d, std::string &out) {
    // vadduwm [VD], [VA], [VB]
    size_t start = out.size();
    out += "vadduwm";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vadduws([[maybe_unused]] const Data &d, std::string &out) {
    // vadduws [VD], [VA], [VB]
    size_t start = out.size();
    out += "vadduws";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vand([[maybe_unused]] const Data &d, std::string &out) {
    // vand [VD], [VA], [VB]
    size_t start = out.size();
    out += "vand";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vand128([[maybe_unused]] const Data &d, std::string &out) {
    // vand128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vand128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vandc([[maybe_unused]] const Data &d, std::string &out) {
    // vandc [VD], [VA], [VB]
    size_t start = out.size();
    out += "vandc";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vandc128([[maybe_unused]] const Data &d, std::string &out) {
    // vandc128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vandc128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vavgsb([[maybe_unused]] const Data &d, std::string &out) {
    // vavgsb [VD], [VA], [VB]
    size_t start = out.size();
    out += "vavgsb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vavgsh([[maybe_unused]] const Data &d, std::string &out) {
    // vavgsh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vavgsh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vavgsw([[maybe_unused]] const Data &d, std::string &out) {
    // vavgsw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vavgsw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vavgub([[maybe_unused]] const Data &d, std::string &out) {
    // vavgub [VD], [VA], [VB]
    size_t start = out.size();
    out += "vavgub";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vavguh([[maybe_unused]] const Data &d, std::string &out) {
    // vavguh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vavguh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vavguw([[maybe_unused]] const Data &d, std::string &out) {
    // vavguw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vavguw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vcfpsxws128([[maybe_unused]] const Data &d, std::string &out) {
    // vcfpsxws128 [VD], [VB], [UIMM]
    size_t start = out.size();
    out += "vcfpsxws128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_3.VB());
    out += ", ";
    hex(out, d.VX128_3.UIMM());
}
void disasm_vcfpuxws128([[maybe_unused]] const Data &d, std::string &out) {
    // vcfpuxws128 [VD], [VB], [UIMM]
    size_t start = out.size();
    out += "vcfpuxws128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_3.VB());
    out += ", ";
    hex(out, d.VX128_3.UIMM());
}
void disasm_vcfsx([[maybe_unused]] const Data &d, std::string &out) {
    // vcfsx [VD], [VB], [UIMM]
    size_t start = out.size();
    out += "vcfsx";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
    out += ", ";
    hex(out, d.VX.UIMM());
}
void disasm_vcfux([[maybe_unused]] const Data &d, std::string &out) {
    // vcfux [VD], [VB], [UIMM]
    size_t start = out.size();
    out += "vcfux";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
    out += ", ";
    hex(out, d.VX.UIMM());
}
void disasm_vcmpbfp([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpbfp[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpbfp";
    if (d.VC.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VC.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VB());
}
void disasm_vcmpbfp128([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpbfp128[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpbfp128";
    if (d.VX128_R.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_R.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_R.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_R.VB());
}
void disasm_vcmpeqfp([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpeqfp[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpeqfp";
    if (d.VC.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VC.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VB());
}
void disasm_vcmpeqfp128([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpeqfp128[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpeqfp128";
    if (d.VX128_R.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_R.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_R.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_R.VB());
}
void disasm_vcmpequb([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpequb[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpequb";
    if (d.VC.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VC.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VB());
}
void disasm_vcmpequh([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpequh[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpequh";
    if (d.VC.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VC.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VB());
}
void disasm_vcmpequw([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpequw[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpequw";
    if (d.VC.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VC.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VB());
}
void disasm_vcmpequw128([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpequw128[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpequw128";
    if (d.VX128_R.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_R.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_R.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_R.VB());
}
void disasm_vcmpgefp([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpgefp[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpgefp";
    if (d.VC.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VC.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VB());
}
void disasm_vcmpgefp128([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpgefp128[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpgefp128";
    if (d.VX128_R.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_R.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_R.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_R.VB());
}
void disasm_vcmpgtfp([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpgtfp[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpgtfp";
    if (d.VC.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VC.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VB());
}
void disasm_vcmpgtfp128([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpgtfp128[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpgtfp128";
    if (d.VX128_R.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_R.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_R.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_R.VB());
}
void disasm_vcmpgtsb([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpgtsb[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpgtsb";
    if (d.VC.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VC.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VB());
}
void disasm_vcmpgtsh([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpgtsh[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpgtsh";
    if (d.VC.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VC.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VB());
}
void disasm_vcmpgtsw([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpgtsw[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpgtsw";
    if (d.VC.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VC.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VB());
}
void disasm_vcmpgtub([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpgtub[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpgtub";
    if (d.VC.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VC.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VB());
}
void disasm_vcmpgtuh([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpgtuh[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpgtuh";
    if (d.VC.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VC.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VB());
}
void disasm_vcmpgtuw([[maybe_unused]] const Data &d, std::string &out) {
    // vcmpgtuw[Rc] [VD], [VA], [VB]
    size_t start = out.size();
    out += "vcmpgtuw";
    if (d.VC.Rc()) out += '.';
    pad(out, start);
    out += "vr" + std::to_string(d.VC.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VC.VB());
}
void disasm_vcsxwfp128([[maybe_unused]] const Data &d, std::string &out) {
    // vcsxwfp128 [VD], [VB], [UIMM]
    size_t start = out.size();
    out += "vcsxwfp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_3.VB());
    out += ", ";
    hex(out, d.VX128_3.UIMM());
}
void disasm_vctsxs([[maybe_unused]] const Data &d, std::string &out) {
    // vctsxs [VD], [VB], [UIMM]
    size_t start = out.size();
    out += "vctsxs";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
    out += ", ";
    hex(out, d.VX.UIMM());
}
void disasm_vctuxs([[maybe_unused]] const Data &d, std::string &out) {
    // vctuxs [VD], [VB], [UIMM]
    size_t start = out.size();
    out += "vctuxs";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
    out += ", ";
    hex(out, d.VX.UIMM());
}
void disasm_vcuxwfp128([[maybe_unused]] const Data &d, std::string &out) {
    // vcuxwfp128 [VD], [VB], [UIMM]
    size_t start = out.size();
    out += "vcuxwfp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_3.VB());
    out += ", ";
    hex(out, d.VX128_3.UIMM());
}
void disasm_vexptefp([[maybe_unused]] const Data &d, std::string &out) {
    // vexptefp [VD], [VB]
    size_t start = out.size();
    out += "vexptefp";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vexptefp128([[maybe_unused]] const Data &d, std::string &out) {
    // vexptefp128 [VD], [VB]
    size_t start = out.size();
    out += "vexptefp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_3.VB());
}
void disasm_vlogefp([[maybe_unused]] const Data &d, std::string &out) {
    // vlogefp [VD], [VB]
    size_t start = out.size();
    out += "vlogefp";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vlogefp128([[maybe_unused]] const Data &d, std::string &out) {
    // vlogefp128 [VD], [VB]
    size_t start = out.size();
    out += "vlogefp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_3.VB());
}
void disasm_vmaddcfp128([[maybe_unused]] const Data &d, std::string &out) {
    // vmaddcfp128 [VD], [VA], [VD], [VB]
    size_t start = out.size();
    out += "vmaddcfp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vmaddfp([[maybe_unused]] const Data &d, std::string &out) {
    // vmaddfp [VD], [VA], [VC], [VB]
    size_t start = out.size();
    out += "vmaddfp";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VC());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
}
void disasm_vmaddfp128([[maybe_unused]] const Data &d, std::string &out) {
    // vmaddfp128 [VD], [VA], [VB], [VD]
    size_t start = out.size();
    out += "vmaddfp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VD());
}
void disasm_vmaxfp([[maybe_unused]] const Data &d, std::string &out) {
    // vmaxfp [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmaxfp";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmaxfp128([[maybe_unused]] const Data &d, std::string &out) {
    // vmaxfp128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmaxfp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vmaxsb([[maybe_unused]] const Data &d, std::string &out) {
    // vmaxsb [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmaxsb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmaxsh([[maybe_unused]] const Data &d, std::string &out) {
    // vmaxsh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmaxsh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmaxsw([[maybe_unused]] const Data &d, std::string &out) {
    // vmaxsw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmaxsw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmaxub([[maybe_unused]] const Data &d, std::string &out) {
    // vmaxub [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmaxub";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmaxuh([[maybe_unused]] const Data &d, std::string &out) {
    // vmaxuh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmaxuh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmaxuw([[maybe_unused]] const Data &d, std::string &out) {
    // vmaxuw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmaxuw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmhaddshs([[maybe_unused]] const Data &d, std::string &out) {
    // vmhaddshs [VD], [VA], [VB], [VC]
    size_t start = out.size();
    out += "vmhaddshs";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VC());
}
void disasm_vmhraddshs([[maybe_unused]] const Data &d, std::string &out) {
    // vmhraddshs [VD], [VA], [VB], [VC]
    size_t start = out.size();
    out += "vmhraddshs";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VC());
}
void disasm_vminfp([[maybe_unused]] const Data &d, std::string &out) {
    // vminfp [VD], [VA], [VB]
    size_t start = out.size();
    out += "vminfp";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vminfp128([[maybe_unused]] const Data &d, std::string &out) {
    // vminfp128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vminfp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vminsb([[maybe_unused]] const Data &d, std::string &out) {
    // vminsb [VD], [VA], [VB]
    size_t start = out.size();
    out += "vminsb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vminsh([[maybe_unused]] const Data &d, std::string &out) {
    // vminsh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vminsh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vminsw([[maybe_unused]] const Data &d, std::string &out) {
    // vminsw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vminsw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vminub([[maybe_unused]] const Data &d, std::string &out) {
    // vminub [VD], [VA], [VB]
    size_t start = out.size();
    out += "vminub";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vminuh([[maybe_unused]] const Data &d, std::string &out) {
    // vminuh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vminuh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vminuw([[maybe_unused]] const Data &d, std::string &out) {
    // vminuw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vminuw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmladduhm([[maybe_unused]] const Data &d, std::string &out) {
    // vmladduhm [VD], [VA], [VB], [VC]
    size_t start = out.size();
    out += "vmladduhm";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VC());
}
void disasm_vmrghb([[maybe_unused]] const Data &d, std::string &out) {
    // vmrghb [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmrghb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmrghh([[maybe_unused]] const Data &d, std::string &out) {
    // vmrghh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmrghh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmrghw([[maybe_unused]] const Data &d, std::string &out) {
    // vmrghw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmrghw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmrghw128([[maybe_unused]] const Data &d, std::string &out) {
    // vmrghw128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmrghw128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vmrglb([[maybe_unused]] const Data &d, std::string &out) {
    // vmrglb [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmrglb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmrglh([[maybe_unused]] const Data &d, std::string &out) {
    // vmrglh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmrglh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmrglw([[maybe_unused]] const Data &d, std::string &out) {
    // vmrglw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmrglw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmrglw128([[maybe_unused]] const Data &d, std::string &out) {
    // vmrglw128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmrglw128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vmsum3fp128([[maybe_unused]] const Data &d, std::string &out) {
    // vmsum3fp128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmsum3fp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vmsum4fp128([[maybe_unused]] const Data &d, std::string &out) {
    // vmsum4fp128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmsum4fp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vmsummbm([[maybe_unused]] const Data &d, std::string &out) {
    // vmsummbm [VD], [VA], [VB], [VC]
    size_t start = out.size();
    out += "vmsummbm";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VC());
}
void disasm_vmsumshm([[maybe_unused]] const Data &d, std::string &out) {
    // vmsumshm [VD], [VA], [VB], [VC]
    size_t start = out.size();
    out += "vmsumshm";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VC());
}
void disasm_vmsumshs([[maybe_unused]] const Data &d, std::string &out) {
    // vmsumshs [VD], [VA], [VB], [VC]
    size_t start = out.size();
    out += "vmsumshs";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VC());
}
void disasm_vmsumubm([[maybe_unused]] const Data &d, std::string &out) {
    // vmsumubm [VD], [VA], [VB], [VC]
    size_t start = out.size();
    out += "vmsumubm";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VC());
}
void disasm_vmsumuhm([[maybe_unused]] const Data &d, std::string &out) {
    // vmsumuhm [VD], [VA], [VB], [VC]
    size_t start = out.size();
    out += "vmsumuhm";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VC());
}
void disasm_vmsumuhs([[maybe_unused]] const Data &d, std::string &out) {
    // vmsumuhs [VD], [VA], [VB], [VC]
    size_t start = out.size();
    out += "vmsumuhs";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VC());
}
void disasm_vmulesb([[maybe_unused]] const Data &d, std::string &out) {
    // vmulesb [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmulesb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmulesh([[maybe_unused]] const Data &d, std::string &out) {
    // vmulesh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmulesh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmuleub([[maybe_unused]] const Data &d, std::string &out) {
    // vmuleub [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmuleub";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmuleuh([[maybe_unused]] const Data &d, std::string &out) {
    // vmuleuh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmuleuh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmulfp128([[maybe_unused]] const Data &d, std::string &out) {
    // vmulfp128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmulfp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vmulosb([[maybe_unused]] const Data &d, std::string &out) {
    // vmulosb [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmulosb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmulosh([[maybe_unused]] const Data &d, std::string &out) {
    // vmulosh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmulosh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmuloub([[maybe_unused]] const Data &d, std::string &out) {
    // vmuloub [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmuloub";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vmulouh([[maybe_unused]] const Data &d, std::string &out) {
    // vmulouh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vmulouh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vnmsubfp([[maybe_unused]] const Data &d, std::string &out) {
    // vnmsubfp [VD], [VA], [VC], [VB]
    size_t start = out.size();
    out += "vnmsubfp";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VC());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
}
void disasm_vnmsubfp128([[maybe_unused]] const Data &d, std::string &out) {
    // vnmsubfp128 [VD], [VA], [VD], [VB]
    size_t start = out.size();
    out += "vnmsubfp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vnor([[maybe_unused]] const Data &d, std::string &out) {
    // vnor [VD], [VA], [VB]
    size_t start = out.size();
    out += "vnor";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vnor128([[maybe_unused]] const Data &d, std::string &out) {
    // vnor128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vnor128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vor([[maybe_unused]] const Data &d, std::string &out) {
    // vor [VD], [VA], [VB]
    size_t start = out.size();
    out += "vor";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vor128([[maybe_unused]] const Data &d, std::string &out) {
    // vor128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vor128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vperm([[maybe_unused]] const Data &d, std::string &out) {
    // vperm [VD], [VA], [VB], [VC]
    size_t start = out.size();
    out += "vperm";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VC());
}
void disasm_vperm128([[maybe_unused]] const Data &d, std::string &out) {
    // vperm128 [VD], [VA], [VB], [VC]
    size_t start = out.size();
    out += "vperm128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_2.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_2.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_2.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_2.VC());
}
void disasm_vpermwi128([[maybe_unused]] const Data &d, std::string &out) {
    // vpermwi128 [VD], [VB], [UIMM]
    size_t start = out.size();
    out += "vpermwi128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_P.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_P.VB());
    out += ", ";
    hex(out, d.VX128_P.UIMM());
}
void disasm_vpkpx([[maybe_unused]] const Data &d, std::string &out) {
    // vpkpx [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkpx";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vpkshss([[maybe_unused]] const Data &d, std::string &out) {
    // vpkshss [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkshss";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vpkshss128([[maybe_unused]] const Data &d, std::string &out) {
    // vpkshss128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkshss128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vpkshus([[maybe_unused]] const Data &d, std::string &out) {
    // vpkshus [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkshus";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vpkshus128([[maybe_unused]] const Data &d, std::string &out) {
    // vpkshus128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkshus128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vpkswss([[maybe_unused]] const Data &d, std::string &out) {
    // vpkswss [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkswss";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vpkswss128([[maybe_unused]] const Data &d, std::string &out) {
    // vpkswss128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkswss128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vpkswus([[maybe_unused]] const Data &d, std::string &out) {
    // vpkswus [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkswus";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vpkswus128([[maybe_unused]] const Data &d, std::string &out) {
    // vpkswus128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkswus128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vpkuhum([[maybe_unused]] const Data &d, std::string &out) {
    // vpkuhum [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkuhum";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vpkuhum128([[maybe_unused]] const Data &d, std::string &out) {
    // vpkuhum128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkuhum128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vpkuhus([[maybe_unused]] const Data &d, std::string &out) {
    // vpkuhus [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkuhus";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vpkuhus128([[maybe_unused]] const Data &d, std::string &out) {
    // vpkuhus128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkuhus128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vpkuwum([[maybe_unused]] const Data &d, std::string &out) {
    // vpkuwum [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkuwum";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vpkuwum128([[maybe_unused]] const Data &d, std::string &out) {
    // vpkuwum128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkuwum128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vpkuwus([[maybe_unused]] const Data &d, std::string &out) {
    // vpkuwus [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkuwus";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vpkuwus128([[maybe_unused]] const Data &d, std::string &out) {
    // vpkuwus128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vpkuwus128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vrefp([[maybe_unused]] const Data &d, std::string &out) {
    // vrefp [VD], [VB]
    size_t start = out.size();
    out += "vrefp";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vrefp128([[maybe_unused]] const Data &d, std::string &out) {
    // vrefp128 [VD], [VB]
    size_t start = out.size();
    out += "vrefp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_3.VB());
}
void disasm_vrfim([[maybe_unused]] const Data &d, std::string &out) {
    // vrfim [VD], [VB]
    size_t start = out.size();
    out += "vrfim";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vrfim128([[maybe_unused]] const Data &d, std::string &out) {
    // vrfim128 [VD], [VB]
    size_t start = out.size();
    out += "vrfim128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_3.VB());
}
void disasm_vrfin([[maybe_unused]] const Data &d, std::string &out) {
    // vrfin [VD], [VB]
    size_t start = out.size();
    out += "vrfin";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vrfin128([[maybe_unused]] const Data &d, std::string &out) {
    // vrfin128 [VD], [VB]
    size_t start = out.size();
    out += "vrfin128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_3.VB());
}
void disasm_vrfip([[maybe_unused]] const Data &d, std::string &out) {
    // vrfip [VD], [VB]
    size_t start = out.size();
    out += "vrfip";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vrfip128([[maybe_unused]] const Data &d, std::string &out) {
    // vrfip128 [VD], [VB]
    size_t start = out.size();
    out += "vrfip128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_3.VB());
}
void disasm_vrfiz([[maybe_unused]] const Data &d, std::string &out) {
    // vrfiz [VD], [VB]
    size_t start = out.size();
    out += "vrfiz";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vrfiz128([[maybe_unused]] const Data &d, std::string &out) {
    // vrfiz128 [VD], [VB]
    size_t start = out.size();
    out += "vrfiz128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_3.VB());
}
void disasm_vrlb([[maybe_unused]] const Data &d, std::string &out) {
    // vrlb [VD], [VA], [VB]
    size_t start = out.size();
    out += "vrlb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vrlh([[maybe_unused]] const Data &d, std::string &out) {
    // vrlh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vrlh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vrlimi128([[maybe_unused]] const Data &d, std::string &out) {
    // vrlimi128 [VD], [VB], [IMM], [z]
    size_t start = out.size();
    out += "vrlimi128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_4.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_4.VB());
    out += ", ";
    dec(out, d.VX128_4.IMM());
    out += ", ";
    dec(out, d.VX128_4.z());
}
void disasm_vrlw([[maybe_unused]] const Data &d, std::string &out) {
    // vrlw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vrlw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vrlw128([[maybe_unused]] const Data &d, std::string &out) {
    // vrlw128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vrlw128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vrsqrtefp([[maybe_unused]] const Data &d, std::string &out) {
    // vrsqrtefp [VD], [VB]
    size_t start = out.size();
    out += "vrsqrtefp";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vrsqrtefp128([[maybe_unused]] const Data &d, std::string &out) {
    // vrsqrtefp128 [VD], [VB]
    size_t start = out.size();
    out += "vrsqrtefp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_3.VB());
}
void disasm_vsel([[maybe_unused]] const Data &d, std::string &out) {
    // vsel [VD], [VA], [VB], [VC]
    size_t start = out.size();
    out += "vsel";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VC());
}
void disasm_vsel128([[maybe_unused]] const Data &d, std::string &out) {
    // vsel128 [VD], [VA], [VB], [VD]
    size_t start = out.size();
    out += "vsel128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VD());
}
void disasm_vsl([[maybe_unused]] const Data &d, std::string &out) {
    // vsl [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsl";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vslb([[maybe_unused]] const Data &d, std::string &out) {
    // vslb [VD], [VA], [VB]
    size_t start = out.size();
    out += "vslb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsldoi([[maybe_unused]] const Data &d, std::string &out) {
    // vsldoi [VD], [VA], [VB], [SHB]
    size_t start = out.size();
    out += "vsldoi";
    pad(out, start);
    out += "vr" + std::to_string(d.VA.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VA.VB());
    out += ", ";
    out += "(UNHANDLED SHB)";
}
void disasm_vsldoi128([[maybe_unused]] const Data &d, std::string &out) {
    // vsldoi128 [VD], [VA], [VB], [SHB]
    size_t start = out.size();
    out += "vsldoi128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_5.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_5.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_5.VB());
    out += ", ";
    out += "(UNHANDLED SHB)";
}
void disasm_vslh([[maybe_unused]] const Data &d, std::string &out) {
    // vslh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vslh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vslo([[maybe_unused]] const Data &d, std::string &out) {
    // vslo [VD], [VA], [VB]
    size_t start = out.size();
    out += "vslo";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vslo128([[maybe_unused]] const Data &d, std::string &out) {
    // vslo128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vslo128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vslw([[maybe_unused]] const Data &d, std::string &out) {
    // vslw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vslw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vslw128([[maybe_unused]] const Data &d, std::string &out) {
    // vslw128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vslw128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vspltb([[maybe_unused]] const Data &d, std::string &out) {
    // vspltb [VD], [VB], [UIMM]
    size_t start = out.size();
    out += "vspltb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
    out += ", ";
    hex(out, d.VX.UIMM());
}
void disasm_vsplth([[maybe_unused]] const Data &d, std::string &out) {
    // vsplth [VD], [VB], [UIMM]
    size_t start = out.size();
    out += "vsplth";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
    out += ", ";
    hex(out, d.VX.UIMM());
}
void disasm_vspltisb([[maybe_unused]] const Data &d, std::string &out) {
    // vspltisb [VD], [SIMM]
    size_t start = out.size();
    out += "vspltisb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    signed_hex(out, d.VX.SIMM());
}
void disasm_vspltish([[maybe_unused]] const Data &d, std::string &out) {
    // vspltish [VD], [SIMM]
    size_t start = out.size();
    out += "vspltish";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    signed_hex(out, d.VX.SIMM());
}
void disasm_vspltisw([[maybe_unused]] const Data &d, std::string &out) {
    // vspltisw [VD], [SIMM]
    size_t start = out.size();
    out += "vspltisw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    signed_hex(out, d.VX.SIMM());
}
void disasm_vspltisw128([[maybe_unused]] const Data &d, std::string &out) {
    // vspltisw128 [VD], [SIMM]
    size_t start = out.size();
    out += "vspltisw128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    signed_hex(out, d.VX128_3.SIMM());
}
void disasm_vspltw([[maybe_unused]] const Data &d, std::string &out) {
    // vspltw [VD], [VB], [UIMM]
    size_t start = out.size();
    out += "vspltw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
    out += ", ";
    hex(out, d.VX.UIMM());
}
void disasm_vspltw128([[maybe_unused]] const Data &d, std::string &out) {
    // vspltw128 [VD], [VB], [UIMM]
    size_t start = out.size();
    out += "vspltw128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128_3.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128_3.VB());
    out += ", ";
    hex(out, d.VX128_3.UIMM());
}
void disasm_vsr([[maybe_unused]] const Data &d, std::string &out) {
    // vsr [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsr";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsrab([[maybe_unused]] const Data &d, std::string &out) {
    // vsrab [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsrab";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsrah([[maybe_unused]] const Data &d, std::string &out) {
    // vsrah [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsrah";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsraw([[maybe_unused]] const Data &d, std::string &out) {
    // vsraw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsraw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsraw128([[maybe_unused]] const Data &d, std::string &out) {
    // vsraw128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsraw128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vsrb([[maybe_unused]] const Data &d, std::string &out) {
    // vsrb [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsrb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsrh([[maybe_unused]] const Data &d, std::string &out) {
    // vsrh [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsrh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsro([[maybe_unused]] const Data &d, std::string &out) {
    // vsro [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsro";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsro128([[maybe_unused]] const Data &d, std::string &out) {
    // vsro128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsro128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vsrw([[maybe_unused]] const Data &d, std::string &out) {
    // vsrw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsrw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsrw128([[maybe_unused]] const Data &d, std::string &out) {
    // vsrw128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsrw128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vsubcuw([[maybe_unused]] const Data &d, std::string &out) {
    // vsubcuw [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsubcuw";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsubfp([[maybe_unused]] const Data &d, std::string &out) {
    // vsubfp [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsubfp";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsubfp128([[maybe_unused]] const Data &d, std::string &out) {
    // vsubfp128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsubfp128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vsubsbs([[maybe_unused]] const Data &d, std::string &out) {
    // vsubsbs [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsubsbs";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsubshs([[maybe_unused]] const Data &d, std::string &out) {
    // vsubshs [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsubshs";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsubsws([[maybe_unused]] const Data &d, std::string &out) {
    // vsubsws [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsubsws";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsububm([[maybe_unused]] const Data &d, std::string &out) {
    // vsububm [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsububm";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsububs([[maybe_unused]] const Data &d, std::string &out) {
    // vsububs [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsububs";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsubuhm([[maybe_unused]] const Data &d, std::string &out) {
    // vsubuhm [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsubuhm";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsubuhs([[maybe_unused]] const Data &d, std::string &out) {
    // vsubuhs [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsubuhs";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsubuwm([[maybe_unused]] const Data &d, std::string &out) {
    // vsubuwm [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsubuwm";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsubuws([[maybe_unused]] const Data &d, std::string &out) {
    // vsubuws [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsubuws";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsum2sws([[maybe_unused]] const Data &d, std::string &out) {
    // vsum2sws [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsum2sws";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsum4sbs([[maybe_unused]] const Data &d, std::string &out) {
    // vsum4sbs [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsum4sbs";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsum4shs([[maybe_unused]] const Data &d, std::string &out) {
    // vsum4shs [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsum4shs";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsum4ubs([[maybe_unused]] const Data &d, std::string &out) {
    // vsum4ubs [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsum4ubs";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vsumsws([[maybe_unused]] const Data &d, std::string &out) {
    // vsumsws [VD], [VA], [VB]
    size_t start = out.size();
    out += "vsumsws";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vupkhpx([[maybe_unused]] const Data &d, std::string &out) {
    // vupkhpx [VD], [VB]
    size_t start = out.size();
    out += "vupkhpx";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vupkhsb([[maybe_unused]] const Data &d, std::string &out) {
    // vupkhsb [VD], [VB]
    size_t start = out.size();
    out += "vupkhsb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vupkhsb128([[maybe_unused]] const Data &d, std::string &out) {
    // vupkhsb128 [VD], [VB]
    size_t start = out.size();
    out += "vupkhsb128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vupkhsh([[maybe_unused]] const Data &d, std::string &out) {
    // vupkhsh [VD], [VB]
    size_t start = out.size();
    out += "vupkhsh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vupklpx([[maybe_unused]] const Data &d, std::string &out) {
    // vupklpx [VD], [VB]
    size_t start = out.size();
    out += "vupklpx";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vupklsb([[maybe_unused]] const Data &d, std::string &out) {
    // vupklsb [VD], [VB]
    size_t start = out.size();
    out += "vupklsb";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vupklsb128([[maybe_unused]] const Data &d, std::string &out) {
    // vupklsb128 [VD], [VB]
    size_t start = out.size();
    out += "vupklsb128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_vupklsh([[maybe_unused]] const Data &d, std::string &out) {
    // vupklsh [VD], [VB]
    size_t start = out.size();
    out += "vupklsh";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vxor([[maybe_unused]] const Data &d, std::string &out) {
    // vxor [VD], [VA], [VB]
    size_t start = out.size();
    out += "vxor";
    pad(out, start);
    out += "vr" + std::to_string(d.VX.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX.VB());
}
void disasm_vxor128([[maybe_unused]] const Data &d, std::string &out) {
    // vxor128 [VD], [VA], [VB]
    size_t start = out.size();
    out += "vxor128";
    pad(out, start);
    out += "vr" + std::to_string(d.VX128.VD());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VA());
    out += ", ";
    out += "vr" + std::to_string(d.VX128.VB());
}
void disasm_xori([[maybe_unused]] const Data &d, std::string &out) {
    // xori [RA], [RS], [UIMM]
    size_t start = out.size();
    out += "xori";
    pad(out, start);
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    out += "r" + std::to_string(d.D.RS());
    out += ", ";
    hex(out, d.D.UIMM());
}
void disasm_xoris([[maybe_unused]] const Data &d, std::string &out) {
    // xoris [RA], [RS], [UIMM]
    size_t start = out.size();
    out += "xoris";
    pad(out, start);
    out += "r" + std::to_string(d.D.RA());
    out += ", ";
    out += "r" + std::to_string(d.D.RS());
    out += ", ";
    hex(out, d.D.UIMM());
}
void disasm_xorx([[maybe_unused]] const Data &d, std::string &out) {
    // xor[Rc] [RA], [RS], [RB]
    size_t start = out.size();
    out += "xor";
    if (d.X.Rc()) out += '.';
    pad(out, start);
    out += "r" + std::to_string(d.X.RA());
    out += ", ";
    out += "r" + std::to_string(d.X.RS());
    out += ", ";
    out += "r" + std::to_string(d.X.RB());
}

// by opcode, null where the name alone is printed
constexpr DisasmFn DISASM[] = {
    disasm_addcx,
    disasm_addex,
    disasm_addi,
    disasm_addic,
    disasm_addicx,
    disasm_addis,
    disasm_addmex,
    disasm_addx,
    disasm_addzex,
    disasm_andcx,
    disasm_andisx,
    disasm_andix,
    disasm_andx,
    disasm_bcctrx,
    disasm_bclrx,
    disasm_bcx,
    disasm_bx,
    disasm_cmp,
    disasm_cmpi,
    disasm_cmpl,
    disasm_cmpli,
    disasm_cntlzdx,
    disasm_cntlzwx,
    disasm_crand,
    disasm_crandc,
    disasm_creqv,
    disasm_crnand,
    disasm_crnor,
    disasm_cror,
    disasm_crorc,
    disasm_crxor,
    disasm_dcbf,
    disasm_dcbi,
    disasm_dcbst,
    disasm_dcbt,
    disasm_dcbtst,
    disasm_dcbz,
    disasm_dcbz128,
    disasm_divdux,
    disasm_divdx,
    disasm_divwux,
    disasm_divwx,
    disasm_eieio,
    disasm_eqvx,
    disasm_extsbx,
    disasm_extshx,
    disasm_extswx,
    disasm_fabsx,
    disasm_faddsx,
    disasm_faddx,
    disasm_fcfidx,
    disasm_fcmpo,
    disasm_fcmpu,
    disasm_fctidx,
    disasm_fctidzx,
    disasm_fctiwx,
    disasm_fctiwzx,
    disasm_fdivsx,
    disasm_fdivx,
    disasm_fmaddsx,
    disasm_fmaddx,
    disasm_fmrx,
    disasm_fmsubsx,
    disasm_fmsubx,
    disasm_fmulsx,
    disasm_fmulx,
    disasm_fnabsx,
    disasm_fnegx,
    disasm_fnmaddsx,
    disasm_fnmaddx,
    disasm_fnmsubsx,
    disasm_fnmsubx,
    disasm_fresx,
    disasm_frspx,
    disasm_frsqrtex,
    disasm_fselx,
    disasm_fsqrtsx,
    disasm_fsqrtx,
    disasm_fsubsx,
    disasm_fsubx,
    disasm_icbi,
    disasm_isync,
    disasm_lbz,
    disasm_lbzu,
    disasm_lbzux,
    disasm_lbzx,
    disasm_ld,
    disasm_ldarx,
    disasm_ldbrx,
    disasm_ldu,
    disasm_ldux,
    disasm_ldx,
    disasm_lfd,
    disasm_lfdu,
    disasm_lfdux,
    disasm_lfdx,
    disasm_lfs,
    disasm_lfsu,
    disasm_lfsux,
    disasm_lfsx,
    disasm_lha,
    disasm_lhau,
    disasm_lhaux,
    disasm_lhax,
    disasm_lhbrx,
    disasm_lhz,
    disasm_lhzu,
    disasm_lhzux,
    disasm_lhzx,
    nullptr,
    nullptr,
    nullptr,
    disasm_lvebx,
    disasm_lvehx,
    disasm_lvewx,
    disasm_lvewx128,
    disasm_lvlx,
    disasm_lvlx128,
    disasm_lvlxl,
    disasm_lvlxl128,
    disasm_lvrx,
    disasm_lvrx128,
    disasm_lvrxl,
    disasm_lvrxl128,
    disasm_lvsl,
    disasm_lvsl128,
    disasm_lvsr,
    disasm_lvsr128,
    disasm_lvx,
    disasm_lvx128,
    disasm_lvxl,
    disasm_lvxl128,
    disasm_lwa,
    disasm_lwarx,
    disasm_lwaux,
    disasm_lwax,
    disasm_lwbrx,
    disasm_lwz,
    disasm_lwzu,
    disasm_lwzux,
    disasm_lwzx,
    disasm_mcrf,
    disasm_mcrfs,
    disasm_mcrxr,
    disasm_mfcr,
    disasm_mffsx,
    disasm_mfmsr,
    disasm_mfspr,
    disasm_mftb,
    nullptr,
    disasm_mtcrf,
    disasm_mtfsb0x,
    disasm_mtfsb1x,
    disasm_mtfsfix,
    disasm_mtfsfx,
    disasm_mtmsr,
    disasm_mtmsrd,
    disasm_mtspr,
    nullptr,
    disasm_mulhdux,
    disasm_mulhdx,
    disasm_mulhwux,
    disasm_mulhwx,
    disasm_mulldx,
    disasm_mulli,
    disasm_mullwx,
    disasm_nandx,
    disasm_negx,
    disasm_norx,
    disasm_orcx,
    disasm_ori,
    disasm_oris,
    disasm_orx,
    disasm_rldclx,
    disasm_rldcrx,
    disasm_rldiclx,
    disasm_rldicrx,
    disasm_rldicx,
    disasm_rldimix,
    disasm_rlwimix,
    disasm_rlwinmx,
    disasm_rlwnmx,
    disasm_sc,
    disasm_sldx,
    disasm_slwx,
    disasm_sradix,
    disasm_sradx,
    disasm_srawix,
    disasm_srawx,
    disasm_srdx,
    disasm_srwx,
    disasm_stb,
    disasm_stbu,
    disasm_stbux,
    disasm_stbx,
    disasm_std,
    disasm_stdbrx,
    disasm_stdcx,
    disasm_stdu,
    disasm_stdux,
    disasm_stdx,
    disasm_stfd,
    disasm_stfdu,
    disasm_stfdux,
    disasm_stfdx,
    disasm_stfiwx,
    disasm_stfs,
    disasm_stfsu,
    disasm_stfsux,
    disasm_stfsx,
    disasm_sth,
    disasm_sthbrx,
    disasm_sthu,
    disasm_sthux,
    disasm_sthx,
    nullptr,
    nullptr,
    nullptr,
    disasm_stvebx,
    disasm_stvehx,
    disasm_stvewx,
    disasm_stvewx128,
    disasm_stvlx,
    disasm_stvlx128,
    disasm_stvlxl,
    disasm_stvlxl128,
    disasm_stvrx,
    disasm_stvrx128,
    disasm_stvrxl,
    disasm_stvrxl128,
    disasm_stvx,
    disasm_stvx128,
    disasm_stvxl,
    disasm_stvxl128,
    disasm_stw,
    disasm_stwbrx,
    disasm_stwcx,
    disasm_stwu,
    disasm_stwux,
    disasm_stwx,
    disasm_subfcx,
    disasm_subfex,
    disasm_subficx,
    disasm_subfmex,
    disasm_subfx,
    disasm_subfzex,
    disasm_sync,
    disasm_td,
    disasm_tdi,
    disasm_tw,
    disasm_twi,
    disasm_vaddcuw,
    disasm_vaddfp,
    disasm_vaddfp128,
    disasm_vaddsbs,
    disasm_vaddshs,
    disasm_vaddsws,
    disasm_vaddubm,
    disasm_vaddubs,
    disasm_vadduhm,
    disasm_vadduhs,
    disasm_vadduwm,
    disasm_vadduws,
    disasm_vand,
    disasm_vand128,
    disasm_vandc,
    disasm_vandc128,
    disasm_vavgsb,
    disasm_vavgsh,
    disasm_vavgsw,
    disasm_vavgub,
    disasm_vavguh,
    disasm_vavguw,
    disasm_vcfpsxws128,
    disasm_vcfpuxws128,
    disasm_vcfsx,
    disasm_vcfux,
    disasm_vcmpbfp,
    disasm_vcmpbfp128,
    disasm_vcmpeqfp,
    disasm_vcmpeqfp128,
    disasm_vcmpequb,
    disasm_vcmpequh,
    disasm_vcmpequw,
    disasm_vcmpequw128,
    disasm_vcmpgefp,
    disasm_vcmpgefp128,
    disasm_vcmpgtfp,
    disasm_vcmpgtfp128,
    disasm_vcmpgtsb,
    disasm_vcmpgtsh,
    disasm_vcmpgtsw,
    disasm_vcmpgtub,
    disasm_vcmpgtuh,
    disasm_vcmpgtuw,
    disasm_vcsxwfp128,
    disasm_vctsxs,
    disasm_vctuxs,
    disasm_vcuxwfp128,
    disasm_vexptefp,
    disasm_vexptefp128,
    disasm_vlogefp,
    disasm_vlogefp128,
    disasm_vmaddcfp128,
    disasm_vmaddfp,
    disasm_vmaddfp128,
    disasm_vmaxfp,
    disasm_vmaxfp128,
    disasm_vmaxsb,
    disasm_vmaxsh,
    disasm_vmaxsw,
    disasm_vmaxub,
    disasm_vmaxuh,
    disasm_vmaxuw,
    disasm_vmhaddshs,
    disasm_vmhraddshs,
    disasm_vminfp,
    disasm_vminfp128,
    disasm_vminsb,
    disasm_vminsh,
    disasm_vminsw,
    disasm_vminub,
    disasm_vminuh,
    disasm_vminuw,
    disasm_vmladduhm,
    disasm_vmrghb,
    disasm_vmrghh,
    disasm_vmrghw,
    disasm_vmrghw128,
    disasm_vmrglb,
    disasm_vmrglh,
    disasm_vmrglw,
    disasm_vmrglw128,
    disasm_vmsum3fp128,
    disasm_vmsum4fp128,
    disasm_vmsummbm,
    disasm_vmsumshm,
    disasm_vmsumshs,
    disasm_vmsumubm,
    disasm_vmsumuhm,
    disasm_vmsumuhs,
    disasm_vmulesb,
    disasm_vmulesh,
    disasm_vmuleub,
    disasm_vmuleuh,
    disasm_vmulfp128,
    disasm_vmulosb,
    disasm_vmulosh,
    disasm_vmuloub,
    disasm_vmulouh,
    disasm_vnmsubfp,
    disasm_vnmsubfp128,
    disasm_vnor,
    disasm_vnor128,
    disasm_vor,
    disasm_vor128,
    disasm_vperm,
    disasm_vperm128,
    disasm_vpermwi128,
    nullptr,
    disasm_vpkpx,
    disasm_vpkshss,
    disasm_vpkshss128,
    disasm_vpkshus,
    disasm_vpkshus128,
    disasm_vpkswss,
    disasm_vpkswss128,
    disasm_vpkswus,
    disasm_vpkswus128,
    disasm_vpkuhum,
    disasm_vpkuhum128,
    disasm_vpkuhus,
    disasm_vpkuhus128,
    disasm_vpkuwum,
    disasm_vpkuwum128,
    disasm_vpkuwus,
    disasm_vpkuwus128,
    disasm_vrefp,
    disasm_vrefp128,
    disasm_vrfim,
    disasm_vrfim128,
    disasm_vrfin,
    disasm_vrfin128,
    disasm_vrfip,
    disasm_vrfip128,
    disasm_vrfiz,
    disasm_vrfiz128,
    disasm_vrlb,
    disasm_vrlh,
    disasm_vrlimi128,
    disasm_vrlw,
    disasm_vrlw128,
    disasm_vrsqrtefp,
    disasm_vrsqrtefp128,
    disasm_vsel,
    disasm_vsel128,
    disasm_vsl,
    disasm_vslb,
    disasm_vsldoi,
    disasm_vsldoi128,
    disasm_vslh,
    disasm_vslo,
    disasm_vslo128,
    disasm_vslw,
    disasm_vslw128,
    disasm_vspltb,
    disasm_vsplth,
    disasm_vspltisb,
    disasm_vspltish,
    disasm_vspltisw,
    disasm_vspltisw128,
    disasm_vspltw,
    disasm_vspltw128,
    disasm_vsr,
    disasm_vsrab,
    disasm_vsrah,
    disasm_vsraw,
    disasm_vsraw128,
    disasm_vsrb,
    disasm_vsrh,
    disasm_vsro,
    disasm_vsro128,
    disasm_vsrw,
    disasm_vsrw128,
    disasm_vsubcuw,
    disasm_vsubfp,
    disasm_vsubfp128,
    disasm_vsubsbs,
    disasm_vsubshs,
    disasm_vsubsws,
    disasm_vsububm,
    disasm_vsububs,
    disasm_vsubuhm,
    disasm_vsubuhs,
    disasm_vsubuwm,
    disasm_vsubuws,
    disasm_vsum2sws,
    disasm_vsum4sbs,
    disasm_vsum4shs,
    disasm_vsum4ubs,
    disasm_vsumsws,
    nullptr,
    disasm_vupkhpx,
    disasm_vupkhsb,
    disasm_vupkhsb128,
    disasm_vupkhsh,
    disasm_vupklpx,
    disasm_vupklsb,
    disasm_vupklsb128,
    disasm_vupklsh,
    disasm_vxor,
    disasm_vxor128,
    disasm_xori,
    disasm_xoris,
    disasm_xorx,
};
