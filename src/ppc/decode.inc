// Written by tools/ppc_gen.py from ppc-instructions.xml (Xenia's, BSD). Do not edit.
// clang-format off

// The body of decode(code): HIT(name) returns that opcode, MISS returns Invalid.
switch (bits(code, 0, 5)) {
case 2: HIT(tdi);
case 3: HIT(twi);
case 4:
    switch ((bits(code, 21, 27) << 4)|(bits(code, 30, 31) << 0)) {
    case 0b00000000011: HIT(lvsl128);
    case 0b00001000011: HIT(lvsr128);
    case 0b00010000011: HIT(lvewx128);
    case 0b00011000011: HIT(lvx128);
    case 0b00110000011: HIT(stvewx128);
    case 0b00111000011: HIT(stvx128);
    case 0b01011000011: HIT(lvxl128);
    case 0b01111000011: HIT(stvxl128);
    case 0b10000000011: HIT(lvlx128);
    case 0b10001000011: HIT(lvrx128);
    case 0b10100000011: HIT(stvlx128);
    case 0b10101000011: HIT(stvrx128);
    case 0b11000000011: HIT(lvlxl128);
    case 0b11001000011: HIT(lvrxl128);
    case 0b11100000011: HIT(stvlxl128);
    case 0b11101000011: HIT(stvrxl128);
    }
    switch ((bits(code, 21, 31) << 0)) {
    case 0b00000000000: HIT(vaddubm);
    case 0b00000000010: HIT(vmaxub);
    case 0b00000000100: HIT(vrlb);
    case 0b00000001000: HIT(vmuloub);
    case 0b00000001010: HIT(vaddfp);
    case 0b00000001100: HIT(vmrghb);
    case 0b00000001110: HIT(vpkuhum);
    case 0b00001000000: HIT(vadduhm);
    case 0b00001000010: HIT(vmaxuh);
    case 0b00001000100: HIT(vrlh);
    case 0b00001001000: HIT(vmulouh);
    case 0b00001001010: HIT(vsubfp);
    case 0b00001001100: HIT(vmrghh);
    case 0b00001001110: HIT(vpkuwum);
    case 0b00010000000: HIT(vadduwm);
    case 0b00010000010: HIT(vmaxuw);
    case 0b00010000100: HIT(vrlw);
    case 0b00010001100: HIT(vmrghw);
    case 0b00010001110: HIT(vpkuhus);
    case 0b00011001110: HIT(vpkuwus);
    case 0b00100000010: HIT(vmaxsb);
    case 0b00100000100: HIT(vslb);
    case 0b00100001000: HIT(vmulosb);
    case 0b00100001010: HIT(vrefp);
    case 0b00100001100: HIT(vmrglb);
    case 0b00100001110: HIT(vpkshus);
    case 0b00101000010: HIT(vmaxsh);
    case 0b00101000100: HIT(vslh);
    case 0b00101001000: HIT(vmulosh);
    case 0b00101001010: HIT(vrsqrtefp);
    case 0b00101001100: HIT(vmrglh);
    case 0b00101001110: HIT(vpkswus);
    case 0b00110000000: HIT(vaddcuw);
    case 0b00110000010: HIT(vmaxsw);
    case 0b00110000100: HIT(vslw);
    case 0b00110001010: HIT(vexptefp);
    case 0b00110001100: HIT(vmrglw);
    case 0b00110001110: HIT(vpkshss);
    case 0b00111000100: HIT(vsl);
    case 0b00111001010: HIT(vlogefp);
    case 0b00111001110: HIT(vpkswss);
    case 0b01000000000: HIT(vaddubs);
    case 0b01000000010: HIT(vminub);
    case 0b01000000100: HIT(vsrb);
    case 0b01000001000: HIT(vmuleub);
    case 0b01000001010: HIT(vrfin);
    case 0b01000001100: HIT(vspltb);
    case 0b01000001110: HIT(vupkhsb);
    case 0b01001000000: HIT(vadduhs);
    case 0b01001000010: HIT(vminuh);
    case 0b01001000100: HIT(vsrh);
    case 0b01001001000: HIT(vmuleuh);
    case 0b01001001010: HIT(vrfiz);
    case 0b01001001100: HIT(vsplth);
    case 0b01001001110: HIT(vupkhsh);
    case 0b01010000000: HIT(vadduws);
    case 0b01010000010: HIT(vminuw);
    case 0b01010000100: HIT(vsrw);
    case 0b01010001010: HIT(vrfip);
    case 0b01010001100: HIT(vspltw);
    case 0b01010001110: HIT(vupklsb);
    case 0b01011000100: HIT(vsr);
    case 0b01011001010: HIT(vrfim);
    case 0b01011001110: HIT(vupklsh);
    case 0b01100000000: HIT(vaddsbs);
    case 0b01100000010: HIT(vminsb);
    case 0b01100000100: HIT(vsrab);
    case 0b01100001000: HIT(vmulesb);
    case 0b01100001010: HIT(vcfux);
    case 0b01100001100: HIT(vspltisb);
    case 0b01100001110: HIT(vpkpx);
    case 0b01101000000: HIT(vaddshs);
    case 0b01101000010: HIT(vminsh);
    case 0b01101000100: HIT(vsrah);
    case 0b01101001000: HIT(vmulesh);
    case 0b01101001010: HIT(vcfsx);
    case 0b01101001100: HIT(vspltish);
    case 0b01101001110: HIT(vupkhpx);
    case 0b01110000000: HIT(vaddsws);
    case 0b01110000010: HIT(vminsw);
    case 0b01110000100: HIT(vsraw);
    case 0b01110001010: HIT(vctuxs);
    case 0b01110001100: HIT(vspltisw);
    case 0b01111001010: HIT(vctsxs);
    case 0b01111001110: HIT(vupklpx);
    case 0b10000000000: HIT(vsububm);
    case 0b10000000010: HIT(vavgub);
    case 0b10000000100: HIT(vand);
    case 0b10000001010: HIT(vmaxfp);
    case 0b10000001100: HIT(vslo);
    case 0b10001000000: HIT(vsubuhm);
    case 0b10001000010: HIT(vavguh);
    case 0b10001000100: HIT(vandc);
    case 0b10001001010: HIT(vminfp);
    case 0b10001001100: HIT(vsro);
    case 0b10010000000: HIT(vsubuwm);
    case 0b10010000010: HIT(vavguw);
    case 0b10010000100: HIT(vor);
    case 0b10011000100: HIT(vxor);
    case 0b10100000010: HIT(vavgsb);
    case 0b10100000100: HIT(vnor);
    case 0b10101000010: HIT(vavgsh);
    case 0b10110000000: HIT(vsubcuw);
    case 0b10110000010: HIT(vavgsw);
    case 0b11000000000: HIT(vsububs);
    case 0b11000000100: HIT(mfvscr);
    case 0b11000001000: HIT(vsum4ubs);
    case 0b11001000000: HIT(vsubuhs);
    case 0b11001000100: HIT(mtvscr);
    case 0b11001001000: HIT(vsum4shs);
    case 0b11010000000: HIT(vsubuws);
    case 0b11010001000: HIT(vsum2sws);
    case 0b11100000000: HIT(vsubsbs);
    case 0b11100001000: HIT(vsum4sbs);
    case 0b11101000000: HIT(vsubshs);
    case 0b11110000000: HIT(vsubsws);
    case 0b11110001000: HIT(vsumsws);
    }
    switch ((bits(code, 22, 31) << 0)) {
    case 0b0000000110: HIT(vcmpequb);
    case 0b0001000110: HIT(vcmpequh);
    case 0b0010000110: HIT(vcmpequw);
    case 0b0011000110: HIT(vcmpeqfp);
    case 0b0111000110: HIT(vcmpgefp);
    case 0b1000000110: HIT(vcmpgtub);
    case 0b1001000110: HIT(vcmpgtuh);
    case 0b1010000110: HIT(vcmpgtuw);
    case 0b1011000110: HIT(vcmpgtfp);
    case 0b1100000110: HIT(vcmpgtsb);
    case 0b1101000110: HIT(vcmpgtsh);
    case 0b1110000110: HIT(vcmpgtsw);
    case 0b1111000110: HIT(vcmpbfp);
    }
    switch ((bits(code, 26, 31) << 0)) {
    case 0b100000: HIT(vmhaddshs);
    case 0b100001: HIT(vmhraddshs);
    case 0b100010: HIT(vmladduhm);
    case 0b100100: HIT(vmsumubm);
    case 0b100101: HIT(vmsummbm);
    case 0b100110: HIT(vmsumuhm);
    case 0b100111: HIT(vmsumuhs);
    case 0b101000: HIT(vmsumshm);
    case 0b101001: HIT(vmsumshs);
    case 0b101010: HIT(vsel);
    case 0b101011: HIT(vperm);
    case 0b101100: HIT(vsldoi);
    case 0b101110: HIT(vmaddfp);
    case 0b101111: HIT(vnmsubfp);
    }
    switch ((bits(code, 27, 27) << 0)) {
    case 0b1: HIT(vsldoi128);
    }
    MISS;
case 5:
    switch ((bits(code, 22, 22) << 5)|(bits(code, 27, 27) << 0)) {
    case 0b000000: HIT(vperm128);
    }
    switch ((bits(code, 22, 25) << 2)|(bits(code, 27, 27) << 0)) {
    case 0b000001: HIT(vaddfp128);
    case 0b000101: HIT(vsubfp128);
    case 0b001001: HIT(vmulfp128);
    case 0b001101: HIT(vmaddfp128);
    case 0b010001: HIT(vmaddcfp128);
    case 0b010101: HIT(vnmsubfp128);
    case 0b011001: HIT(vmsum3fp128);
    case 0b011101: HIT(vmsum4fp128);
    case 0b100000: HIT(vpkshss128);
    case 0b100001: HIT(vand128);
    case 0b100100: HIT(vpkshus128);
    case 0b100101: HIT(vandc128);
    case 0b101000: HIT(vpkswss128);
    case 0b101001: HIT(vnor128);
    case 0b101100: HIT(vpkswus128);
    case 0b101101: HIT(vor128);
    case 0b110000: HIT(vpkuhum128);
    case 0b110001: HIT(vxor128);
    case 0b110100: HIT(vpkuhus128);
    case 0b110101: HIT(vsel128);
    case 0b111000: HIT(vpkuwum128);
    case 0b111001: HIT(vslo128);
    case 0b111100: HIT(vpkuwus128);
    case 0b111101: HIT(vsro128);
    }
    MISS;
case 6:
    switch ((bits(code, 21, 22) << 5)|(bits(code, 26, 27) << 0)) {
    case 0b0100001: HIT(vpermwi128);
    }
    switch ((bits(code, 21, 23) << 4)|(bits(code, 26, 27) << 0)) {
    case 0b1100001: HIT(vpkd3d128);
    case 0b1110001: HIT(vrlimi128);
    }
    switch ((bits(code, 21, 27) << 0)) {
    case 0b0100011: HIT(vcfpsxws128);
    case 0b0100111: HIT(vcfpuxws128);
    case 0b0101011: HIT(vcsxwfp128);
    case 0b0101111: HIT(vcuxwfp128);
    case 0b0110011: HIT(vrfim128);
    case 0b0110111: HIT(vrfin128);
    case 0b0111011: HIT(vrfip128);
    case 0b0111111: HIT(vrfiz128);
    case 0b1100011: HIT(vrefp128);
    case 0b1100111: HIT(vrsqrtefp128);
    case 0b1101011: HIT(vexptefp128);
    case 0b1101111: HIT(vlogefp128);
    case 0b1110011: HIT(vspltw128);
    case 0b1110111: HIT(vspltisw128);
    case 0b1111111: HIT(vupkd3d128);
    }
    switch ((bits(code, 22, 24) << 3)|(bits(code, 27, 27) << 0)) {
    case 0b000000: HIT(vcmpeqfp128);
    case 0b001000: HIT(vcmpgefp128);
    case 0b010000: HIT(vcmpgtfp128);
    case 0b011000: HIT(vcmpbfp128);
    case 0b100000: HIT(vcmpequw128);
    }
    switch ((bits(code, 22, 25) << 2)|(bits(code, 27, 27) << 0)) {
    case 0b000101: HIT(vrlw128);
    case 0b001101: HIT(vslw128);
    case 0b010101: HIT(vsraw128);
    case 0b011101: HIT(vsrw128);
    case 0b101000: HIT(vmaxfp128);
    case 0b101100: HIT(vminfp128);
    case 0b110000: HIT(vmrghw128);
    case 0b110100: HIT(vmrglw128);
    case 0b111000: HIT(vupkhsb128);
    case 0b111100: HIT(vupklsb128);
    }
    MISS;
case 7: HIT(mulli);
case 8: HIT(subficx);
case 10: HIT(cmpli);
case 11: HIT(cmpi);
case 12: HIT(addic);
case 13: HIT(addicx);
case 14: HIT(addi);
case 15: HIT(addis);
case 16: HIT(bcx);
case 17: HIT(sc);
case 18: HIT(bx);
case 19:
    switch ((bits(code, 21, 30) << 0)) {
    case 0b0000000000: HIT(mcrf);
    case 0b0000010000: HIT(bclrx);
    case 0b0000100001: HIT(crnor);
    case 0b0010000001: HIT(crandc);
    case 0b0010010110: HIT(isync);
    case 0b0011000001: HIT(crxor);
    case 0b0011100001: HIT(crnand);
    case 0b0100000001: HIT(crand);
    case 0b0100100001: HIT(creqv);
    case 0b0110100001: HIT(crorc);
    case 0b0111000001: HIT(cror);
    case 0b1000010000: HIT(bcctrx);
    }
    MISS;
case 20: HIT(rlwimix);
case 21: HIT(rlwinmx);
case 23: HIT(rlwnmx);
case 24: HIT(ori);
case 25: HIT(oris);
case 26: HIT(xori);
case 27: HIT(xoris);
case 28: HIT(andix);
case 29: HIT(andisx);
case 30:
    switch ((bits(code, 27, 29) << 0)) {
    case 0b000: HIT(rldiclx);
    case 0b001: HIT(rldicrx);
    case 0b010: HIT(rldicx);
    case 0b011: HIT(rldimix);
    }
    switch ((bits(code, 27, 30) << 0)) {
    case 0b1000: HIT(rldclx);
    case 0b1001: HIT(rldcrx);
    }
    MISS;
case 31:
    switch ((bits(code, 21, 29) << 0)) {
    case 0b110011101: HIT(sradix);
    }
    switch ((bits(code, 21, 30) << 0)) {
    case 0b0000000000: HIT(cmp);
    case 0b0000000100: HIT(tw);
    case 0b0000000110: HIT(lvsl);
    case 0b0000000111: HIT(lvebx);
    case 0b0000010011: HIT(mfcr);
    case 0b0000010100: HIT(lwarx);
    case 0b0000010101: HIT(ldx);
    case 0b0000010111: HIT(lwzx);
    case 0b0000011000: HIT(slwx);
    case 0b0000011010: HIT(cntlzwx);
    case 0b0000011011: HIT(sldx);
    case 0b0000011100: HIT(andx);
    case 0b0000100000: HIT(cmpl);
    case 0b0000100110: HIT(lvsr);
    case 0b0000100111: HIT(lvehx);
    case 0b0000110101: HIT(ldux);
    case 0b0000110110: HIT(dcbst);
    case 0b0000110111: HIT(lwzux);
    case 0b0000111010: HIT(cntlzdx);
    case 0b0000111100: HIT(andcx);
    case 0b0001000100: HIT(td);
    case 0b0001000111: HIT(lvewx);
    case 0b0001010011: HIT(mfmsr);
    case 0b0001010100: HIT(ldarx);
    case 0b0001010110: HIT(dcbf);
    case 0b0001010111: HIT(lbzx);
    case 0b0001100111: HIT(lvx);
    case 0b0001110111: HIT(lbzux);
    case 0b0001111100: HIT(norx);
    case 0b0010000111: HIT(stvebx);
    case 0b0010010000: HIT(mtcrf);
    case 0b0010010010: HIT(mtmsr);
    case 0b0010010101: HIT(stdx);
    case 0b0010010110: HIT(stwcx);
    case 0b0010010111: HIT(stwx);
    case 0b0010100111: HIT(stvehx);
    case 0b0010110010: HIT(mtmsrd);
    case 0b0010110101: HIT(stdux);
    case 0b0010110111: HIT(stwux);
    case 0b0011000111: HIT(stvewx);
    case 0b0011010110: HIT(stdcx);
    case 0b0011010111: HIT(stbx);
    case 0b0011100111: HIT(stvx);
    case 0b0011110110: HIT(dcbtst);
    case 0b0011110111: HIT(stbux);
    case 0b0100010110: HIT(dcbt);
    case 0b0100010111: HIT(lhzx);
    case 0b0100011100: HIT(eqvx);
    case 0b0100110111: HIT(lhzux);
    case 0b0100111100: HIT(xorx);
    case 0b0101010011: HIT(mfspr);
    case 0b0101010101: HIT(lwax);
    case 0b0101010111: HIT(lhax);
    case 0b0101100111: HIT(lvxl);
    case 0b0101110011: HIT(mftb);
    case 0b0101110101: HIT(lwaux);
    case 0b0101110111: HIT(lhaux);
    case 0b0110010111: HIT(sthx);
    case 0b0110011100: HIT(orcx);
    case 0b0110110111: HIT(sthux);
    case 0b0110111100: HIT(orx);
    case 0b0111010011: HIT(mtspr);
    case 0b0111010110: HIT(dcbi);
    case 0b0111011100: HIT(nandx);
    case 0b0111100111: HIT(stvxl);
    case 0b1000000000: HIT(mcrxr);
    case 0b1000000111: HIT(lvlx);
    case 0b1000010100: HIT(ldbrx);
    case 0b1000010101: HIT(lswx);
    case 0b1000010110: HIT(lwbrx);
    case 0b1000010111: HIT(lfsx);
    case 0b1000011000: HIT(srwx);
    case 0b1000011011: HIT(srdx);
    case 0b1000100111: HIT(lvrx);
    case 0b1000110111: HIT(lfsux);
    case 0b1001010101: HIT(lswi);
    case 0b1001010110: HIT(sync);
    case 0b1001010111: HIT(lfdx);
    case 0b1001110111: HIT(lfdux);
    case 0b1010000111: HIT(stvlx);
    case 0b1010010100: HIT(stdbrx);
    case 0b1010010101: HIT(stswx);
    case 0b1010010110: HIT(stwbrx);
    case 0b1010010111: HIT(stfsx);
    case 0b1010100111: HIT(stvrx);
    case 0b1010110111: HIT(stfsux);
    case 0b1011010101: HIT(stswi);
    case 0b1011010111: HIT(stfdx);
    case 0b1011110111: HIT(stfdux);
    case 0b1100000111: HIT(lvlxl);
    case 0b1100010110: HIT(lhbrx);
    case 0b1100011000: HIT(srawx);
    case 0b1100011010: HIT(sradx);
    case 0b1100100111: HIT(lvrxl);
    case 0b1100111000: HIT(srawix);
    case 0b1101010110: HIT(eieio);
    case 0b1110000111: HIT(stvlxl);
    case 0b1110010110: HIT(sthbrx);
    case 0b1110011010: HIT(extshx);
    case 0b1110100111: HIT(stvrxl);
    case 0b1110111010: HIT(extsbx);
    case 0b1111010110: HIT(icbi);
    case 0b1111010111: HIT(stfiwx);
    case 0b1111011010: HIT(extswx);
    }
    switch ((bits(code, 22, 30) << 0)) {
    case 0b000001000: HIT(subfcx);
    case 0b000001001: HIT(mulhdux);
    case 0b000001010: HIT(addcx);
    case 0b000001011: HIT(mulhwux);
    case 0b000101000: HIT(subfx);
    case 0b001001001: HIT(mulhdx);
    case 0b001001011: HIT(mulhwx);
    case 0b001101000: HIT(negx);
    case 0b010001000: HIT(subfex);
    case 0b010001010: HIT(addex);
    case 0b011001000: HIT(subfzex);
    case 0b011001010: HIT(addzex);
    case 0b011101000: HIT(subfmex);
    case 0b011101001: HIT(mulldx);
    case 0b011101010: HIT(addmex);
    case 0b011101011: HIT(mullwx);
    case 0b100001010: HIT(addx);
    case 0b111001001: HIT(divdux);
    case 0b111001011: HIT(divwux);
    case 0b111101001: HIT(divdx);
    case 0b111101011: HIT(divwx);
    }
    switch ((bits(code, 6, 10) << 20)|(bits(code, 21, 30) << 0)) {
    case 0b0000000000000001111110110: HIT(dcbz);
    case 0b0000100000000001111110110: HIT(dcbz128);
    }
    MISS;
case 32: HIT(lwz);
case 33: HIT(lwzu);
case 34: HIT(lbz);
case 35: HIT(lbzu);
case 36: HIT(stw);
case 37: HIT(stwu);
case 38: HIT(stb);
case 39: HIT(stbu);
case 40: HIT(lhz);
case 41: HIT(lhzu);
case 42: HIT(lha);
case 43: HIT(lhau);
case 44: HIT(sth);
case 45: HIT(sthu);
case 46: HIT(lmw);
case 47: HIT(stmw);
case 48: HIT(lfs);
case 49: HIT(lfsu);
case 50: HIT(lfd);
case 51: HIT(lfdu);
case 52: HIT(stfs);
case 53: HIT(stfsu);
case 54: HIT(stfd);
case 55: HIT(stfdu);
case 58:
    switch ((bits(code, 30, 31) << 0)) {
    case 0b00: HIT(ld);
    case 0b01: HIT(ldu);
    case 0b10: HIT(lwa);
    }
    MISS;
case 59:
    switch ((bits(code, 26, 30) << 0)) {
    case 0b10010: HIT(fdivsx);
    case 0b10100: HIT(fsubsx);
    case 0b10101: HIT(faddsx);
    case 0b10110: HIT(fsqrtsx);
    case 0b11000: HIT(fresx);
    case 0b11001: HIT(fmulsx);
    case 0b11100: HIT(fmsubsx);
    case 0b11101: HIT(fmaddsx);
    case 0b11110: HIT(fnmsubsx);
    case 0b11111: HIT(fnmaddsx);
    }
    MISS;
case 62:
    switch ((bits(code, 30, 31) << 0)) {
    case 0b00: HIT(std);
    case 0b01: HIT(stdu);
    }
    MISS;
case 63:
    switch ((bits(code, 21, 30) << 0)) {
    case 0b0000000000: HIT(fcmpu);
    case 0b0000001100: HIT(frspx);
    case 0b0000001110: HIT(fctiwx);
    case 0b0000001111: HIT(fctiwzx);
    case 0b0000100000: HIT(fcmpo);
    case 0b0000100110: HIT(mtfsb1x);
    case 0b0000101000: HIT(fnegx);
    case 0b0001000000: HIT(mcrfs);
    case 0b0001000110: HIT(mtfsb0x);
    case 0b0001001000: HIT(fmrx);
    case 0b0010000110: HIT(mtfsfix);
    case 0b0010001000: HIT(fnabsx);
    case 0b0100001000: HIT(fabsx);
    case 0b1001000111: HIT(mffsx);
    case 0b1011000111: HIT(mtfsfx);
    case 0b1100101110: HIT(fctidx);
    case 0b1100101111: HIT(fctidzx);
    case 0b1101001110: HIT(fcfidx);
    }
    switch ((bits(code, 26, 30) << 0)) {
    case 0b10010: HIT(fdivx);
    case 0b10100: HIT(fsubx);
    case 0b10101: HIT(faddx);
    case 0b10110: HIT(fsqrtx);
    case 0b10111: HIT(fselx);
    case 0b11001: HIT(fmulx);
    case 0b11010: HIT(frsqrtex);
    case 0b11100: HIT(fmsubx);
    case 0b11101: HIT(fmaddx);
    case 0b11110: HIT(fnmsubx);
    case 0b11111: HIT(fnmaddx);
    }
    MISS;
default: MISS;
}
