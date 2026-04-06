OPENQASM 2.0;

//  author: Suhas Vittal
// 
//  This is Quicksilver's special `qelib1.inc` for its basis gate set:
//      h, x, y, z, s, sdg, t, tdg, rx, ry, rz, cx, cz, ccx, ccz, measure
//  Note that this isn't the standard gate set assumed for OpenQASM 2.0.
// 

gate id a
{
    // NOP
}

gate reset a
{  
    // this is a nop for simulation purposes (X-based reset can occur in software)
}

gate cswap a, b, c
{
    cx c, b;
    ccx a, b, c;
    cx c, b;
}

gate u(X,Y,Z) a
{
    rz(Z) a;
    sx a;
    z a;
    rz(X) a;
    sx a;
    z a;
    rz(Y) a;
}

gate p(X) a
{
    rz(X) a; 
}

gate cp(X) a, b
{
    rz(X/2) a;
    cx a, b;
    rz(-X/2) b;
    cx a, b;
    rz(X/2) b;
}

gate ccp(X) a, b, c
{
    cp(X/2)     a, b;
    ccx         a, b, c;
    rz(-X/2)    c;
    ccx         a, b, c;
    rz(X/2)     c;
}

gate ch a, b
{
    sxdg b;
    tdg b;
    cx a, b;
    t b;
    sx b;
}

gate cy a, b
{
    sdg b;
    cx a, b;
    s b;
}

//////////////////////////////////////
//  CONTROLLED UNITARY GATES        //
//////////////////////////////////////

gate cu(X,Y,Z,W) a, b
{
    rz(W + (Y+Z)/2) a;
    rz((Z-Y)/2)     b;

    cx a, b;

    rz((-Y-Z)/2) b;
    sx           b;
    z            b;
    rz(-X/2)     b;
    sx           b;
    z            b;
    
    cx a, b;

    sx           b;
    z            b;
    rz(X/2)      b;
    sx           b;
    z            b;
    rz(Y)        b;
}

gate cu1(X) a, b
{
    rz(X/2) a;
    cx a, b;
    rz(-X/2) b;
    cx a, b;
    rz (X/2) b;
}

gate cu3(X,Y,Z) a, b
{
    cu(X,Y,Z,0) a, b;
}

//////////////////////////////////////
//  RY AND CTRL'd ROTATION GATES    //
//////////////////////////////////////

gate ry(X) a
{
    sdg a;
    rx(X) a;
    s a;
}

gate crx(X) a, b
{
    h b;
    rx(X/2) b;
    cx a, b;
    rx(-X/2) b;
    cx a, b;
    h b;
}

gate cry(X) a, b
{
    sx b;
    rz(X/2) b;
    cx a, b;
    rz(-X/2) b;
    cx a, b;
    sxdg b;
}

gate crz(X) a, b
{
    rz(X/2) b;
    cx a, b;
    rz(-X/2) b;
    cx a, b;
}

//////////////////////////////////////
//  Ux GATES (for compatibility)    //
//////////////////////////////////////

gate u1(X) a
{
    rz(X) a;
}

gate u2(X,Y) a
{
    sdg a;
    rz(Y) a;
    sx a;
    rz(X) a;
    s a;
}

gate u3(X,Y,Z) a
{
    u(X,Y,Z) a;
}

//////////////////////////////////////
// RZZ, RZX, etc.                   //
//////////////////////////////////////

gate rzz(X) a, b
{
    cx a, b;
    rz(X) b;
    cx a, b;
}

gate rzx(X) a, b
{
    h b;
    rzz(X) a, b;
    h b;
}

gate ryy(X) a, b
{
    sx a;
    sx b;
    rzz(X) a, b;
    sxdg a;
    sxdg b;
}

gate rxx(X) a, b
{
    h a;
    h b;
    rzz(X) a, b;
    h a;
    h b;
}
// two-qubit ZZ rotation
gate rzz(theta) a,b
{
  cx a,b;
  u1(theta) b;
  cx a,b;
}
// relative-phase CCX
gate rccx a,b,c
{
  u2(0,pi) c;
  u1(pi/4) c;
  cx b, c;
  u1(-pi/4) c;
  cx a, c;
  u1(pi/4) c;
  cx b, c;
  u1(-pi/4) c;
  u2(0,pi) c;
}
// relative-phase 3-controlled X gate
gate rc3x a,b,c,d
{
  u2(0,pi) d;
  u1(pi/4) d;
  cx c,d;
  u1(-pi/4) d;
  u2(0,pi) d;
  cx a,d;
  u1(pi/4) d;
  cx b,d;
  u1(-pi/4) d;
  cx a,d;
  u1(pi/4) d;
  cx b,d;
  u1(-pi/4) d;
  u2(0,pi) d;
  u1(pi/4) d;
  cx c,d;
  u1(-pi/4) d;
  u2(0,pi) d;
}
// 3-controlled X gate
gate c3x a,b,c,d
{
    h d;
    p(pi/8) a;
    p(pi/8) b;
    p(pi/8) c;
    p(pi/8) d;
    cx a, b;
    p(-pi/8) b;
    cx a, b;
    cx b, c;
    p(-pi/8) c;
    cx a, c;
    p(pi/8) c;
    cx b, c;
    p(-pi/8) c;
    cx a, c;
    cx c, d;
    p(-pi/8) d;
    cx b, d;
    p(pi/8) d;
    cx c, d;
    p(-pi/8) d;
    cx a, d;
    p(pi/8) d;
    cx c, d;
    p(-pi/8) d;
    cx b, d;
    p(pi/8) d;
    cx c, d;
    p(-pi/8) d;
    cx a, d;
    h d;
}
// 3-controlled sqrt(X) gate, this equals the C3X gate where the CU1 rotations are -pi/8 not -pi/4
gate c3sqrtx a,b,c,d
{
    h d; cu1(pi/8) a,d; h d;
    cx a,b;
    h d; cu1(-pi/8) b,d; h d;
    cx a,b;
    h d; cu1(pi/8) b,d; h d;
    cx b,c;
    h d; cu1(-pi/8) c,d; h d;
    cx a,c;
    h d; cu1(pi/8) c,d; h d;
    cx b,c;
    h d; cu1(-pi/8) c,d; h d;
    cx a,c;
    h d; cu1(pi/8) c,d; h d;
}
// 4-controlled X gate
gate c4x a,b,c,d,e
{
    h e; cu1(pi/2) d,e; h e;
    c3x a,b,c,d;
    h e; cu1(-pi/2) d,e; h e;
    c3x a,b,c,d;
    c3sqrtx a,b,c,e;
}
