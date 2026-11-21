#ifndef GXM_RECON_H
#define GXM_RECON_H

/*
 * Diagnostic only. If ux0:data/adrenaline_gxm_recon.flag exists, hooks the SceGxm
 * imports of ScePspemu and writes every new set of render target / surface /
 * viewport parameters to ux0:data/adrenaline_gxm_recon.txt. The result shows
 * whether the PSP GE is rendered by ScePspemu through GXM and at what size.
 * Without the flag file nothing is hooked.
 */
void GxmReconInit(void);
void GxmReconExit(void);

#endif
