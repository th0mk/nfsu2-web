/* setting.h -- settings from the environment: settings.ini's lines as the
 * page or the Mac app passes them (NFSU2_*), and test knobs.
 * No guest memory here (metal.m uses it too). */
#ifndef NFSU2_SETTING_H
#define NFSU2_SETTING_H
#include <stdlib.h>
#include <strings.h>

/* getenv, looked up once per call site and kept.  For hot paths: a getenv
 * per call measurably cost (getenv + __findenv_locked were 3 % of the guest
 * thread in a profile), and the environment does not change during a run. */
#define ENVC(nm_) ({ static const char *_ev_; static int _ei_; \
                     if (!_ei_){ _ei_ = 1; _ev_ = getenv(nm_); } _ev_; })

/* An on/off setting (the page writes 1 or 0; the Mac app passes the line's
 * own on/off): unset or empty, dflt; 0..., off, no or false (any case), 0;
 * anything else, 1.  Read each call: a caller that reads it often keeps it. */
static inline int setting_bool(const char *env, int dflt){
    const char *e = getenv(env);
    if (!e || !*e) return dflt;
    return *e != '0' && strcasecmp(e, "off") && strcasecmp(e, "no") && strcasecmp(e, "false");
}
#endif
