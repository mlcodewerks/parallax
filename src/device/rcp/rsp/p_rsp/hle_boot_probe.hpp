/* Functional model of the SDK vector self-test in the known boot stubs.
 * Uses the native vector primitives; no instruction-cycle measurement. */
#ifndef PARALLAX_HLE_BOOT_PROBE_HPP
#define PARALLAX_HLE_BOOT_PROBE_HPP
#include "rsp_op.hpp"
#include "dpc_bridge.h"
static uint32_t rsp_hle_boot_probe(const uint32_t *im, const uint32_t *dm,
    uint32_t *state, uint16_t *vco)
{
    if(im[1]!=0x8c11ffc4u || im[0x44/4]!=0x3c010080u ||
        im[0x48/4]!=0x340303d8u)return 0;
    bool rare=im[0x80/4]==0x4a6d6b54u && im[0x84/4]==0x4acd6b54u &&
        im[0x88/4]==0x4b8d6b54u && im[0x50/4]==0x201010f8u &&
        im[0xc8/4]==0x201010f0u && im[0xec/4]==0x40801800u;
    bool zelda=im[0x80/4]==0x4a6d6b6cu && im[0x84/4]==0x4acd6b54u &&
        im[0x88/4]==0x4b8d6b6du && im[0x50/4]==0x201010d0u &&
        im[0x9c/4]==0x201010c8u && im[0xc0/4]==0x40801800u;
    /* The shorter VADDC test omits the following LQV/VNE memory test.
     * Its success path selects the preceding eight-byte entry pair. */
    bool short_add=im[0x80/4]==0x4a6d6b54u && im[0x84/4]==0x4acd6b54u &&
        im[0x88/4]==0x4b8d6b54u && im[0x50/4]==0x201010d8u &&
        im[0xa0/4]==0 && im[0xa8/4]==0x201010d0u &&
        im[0xcc/4]==0x40801800u;
    if(!rare && !zelda && !short_add)return 0;
    uint32_t source=rare?0xf8u:short_add?0xd8u:0xd0u;
    if(im[0]!=0x08000411u) {
        /* Both replacement entries jump beyond the DPC_CLOCK block. */
        if(short_add)return 0;
        if(im[0]==im[source/4])return 0x80000u;
        if(im[0]==im[(source-8u)/4])return 0x180000u;
        return 0;
    }
    const uint32_t *gpr=state+5;
    if(gpr[11]!=0x3d8u || gpr[4]!=0x03a04820u || gpr[6]!=0x25290004u ||
        (im[0x74/4] && (gpr[5]>>12)<76u))return 0x10000u|source;
    bool upper_branch=im[0x7c/4] && (gpr[5]>>12)>79u;
    RSP::CPUState s{};
    s.dmem=const_cast<uint32_t*>(dm);
    const uint32_t *p=state+PARALLEL_RSP_STATE_VECTOR_OFFSET;
    for(auto &reg:s.cp2.regs)for(auto &v:reg.e)v=uint16_t(*p++);
    for(auto &flag:s.cp2.flags)for(auto &v:flag.e)v=uint16_t(*p++);
    for(auto &v:s.cp2.acc.e)v=uint16_t(*p++);
    rsp_set_flags(s.cp2.flags[RSP::RSP_VCO].e,*vco);
    bool accepted=false;
    if(rare || short_add) {
        RSP_VADDC(&s,13,13,13,3); /* upper-bound branch delay slot */
        if(!upper_branch) {
        RSP_VADDC(&s,13,13,13,6);RSP_VADDC(&s,13,13,13,12);
        uint32_t test=uint32_t(int32_t(int16_t(s.cp2.regs[13].e[0])))+uint32_t(int32_t(int16_t(im[0x90/4])));
        RSP_VADD(&s,13,13,13,0); /* delay slot executes on either branch */
        if((test&0xfff0u)==0) {
            if(short_add)accepted=true;
            else {
            RSP_LQV(&s,12,0,0,0);
            if(((uint32_t(int32_t(int16_t(s.cp2.regs[12].e[0])))+1u)&0xfffeu)!=0)accepted=true;
            RSP_VNE(&s,12,12,12,8);
            if(rsp_get_flags(s.cp2.flags[RSP::RSP_VCC].e))accepted=true;
            }
        }
        }
    } else {
        RSP_VXOR(&s,13,13,13,3); /* upper-bound branch delay slot */
        if(!upper_branch) {
            RSP_VADDC(&s,13,13,13,6);RSP_VNXOR(&s,13,13,13,12);
            accepted=true;
        }
    }
    uint32_t *out=state+PARALLEL_RSP_STATE_VECTOR_OFFSET;
    for(auto &reg:s.cp2.regs)for(auto v:reg.e)*out++=v;
    for(auto &flag:s.cp2.flags)for(auto v:flag.e)*out++=v;
    for(auto v:s.cp2.acc.e)*out++=v;
    *vco=uint16_t(rsp_get_flags(s.cp2.flags[RSP::RSP_VCO].e));
    return 0x50000u|(accepted?0x20000u:0u)|(accepted?source-8u:source);
}
#endif
