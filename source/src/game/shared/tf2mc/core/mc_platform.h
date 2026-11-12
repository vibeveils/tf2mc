//========= TF2MC bridge - engine independent core =========//
// The Source SDK project files define sprintf/strncpy/_snprintf/fopen to poison
// identifiers, which breaks <cstring>/<cstdio> in the standard library. The core
// only uses the safe functions (snprintf, memcpy, sscanf), so lift the poison.
//==========================================================//
#pragma once
#ifdef sprintf
#undef sprintf
#endif
#ifdef strncpy
#undef strncpy
#endif
#ifdef _snprintf
#undef _snprintf
#endif
#ifdef fopen
#undef fopen
#endif
