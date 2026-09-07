/* ============================================================
 * ext-protocol/all.h — external/extra Wayland protocol support.
 *
 * Pulls in the protocol implementations mango adds on top of core wlroots:
 *   - ext-workspace.h     : expose tags as workspace groups (for bars)
 *   - foreign-toplevel.h  : publish clients to external taskbars
 *   - hdr.h               : HDR output / render-format negotiation
 *   - tearing.h           : allow tearing (low-latency) presentation
 *   - text-input.h        : input-method relay (IME / on-screen keyboards)
 *   - xdg-activation.h    : token-based focus activation (launchers)
 *   - xdg-output.h        : report logical output geometry to clients
 * Each header is self-contained around a wlr_* manager.
 * ============================================================ */

#include "ext-workspace.h"
#include "foreign-toplevel.h"
#include "hdr.h"
#include "tearing.h"
#include "text-input.h"
#include "xdg-activation.h"
#include "xdg-output.h"
