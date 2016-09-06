#include <dde/dde.h>
#include "r_dopri.h"
#include "r_difeq.h"

#include <R.h>
#include <Rinternals.h>
#include <R_ext/Rdynload.h>

static const R_CallMethodDef call_methods[] = {
  {"Cdopri",  (DL_FUNC) &r_dopri,  19},
  {"Cylag",   (DL_FUNC) &r_ylag,    2},

  {"Cdifeq",  (DL_FUNC) &r_difeq,  11},

  {NULL,      NULL,                 0}
};

void R_init_dde(DllInfo *info) {
  // Register C routines to be called from C:
  R_RegisterCCallable("dde", "ylag_1",       (DL_FUNC) &ylag_1);
  R_RegisterCCallable("dde", "ylag_all",     (DL_FUNC) &ylag_all);
  R_RegisterCCallable("dde", "ylag_vec",     (DL_FUNC) &ylag_vec);
  R_RegisterCCallable("dde", "ylag_vec_int", (DL_FUNC) &ylag_vec_int);
  // Register C routines to be called from R:
  R_registerRoutines(info, NULL, call_methods, NULL, NULL);
}