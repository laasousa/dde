#include "dopri.h"
#include "dopri_5.h"
#include <R.h>

dopri_data* dopri_data_alloc(deriv_func* target, size_t n,
                             output_func* output, size_t n_out,
                             void *data, size_t n_history) {
  dopri_data *ret = (dopri_data*) R_Calloc(1, dopri_data);
  ret->target = target;
  ret->output = output;
  ret->data = data;

  ret->method = DOPRI_5;
  ret->order = ret->method == DOPRI_5 ? 5 : 8;

  ret->n = n;
  ret->n_out = n_out;

  ret->n_times = 0;
  ret->times = NULL;
  // ret->times_idx -- set in reset

  // tcrit variables are set in reset

  // State vectors
  ret->y0 = R_Calloc(n, double); // initial
  ret->y  = R_Calloc(n, double); // current
  ret->y1 = R_Calloc(n, double); // next

  // NOTE: There's no real reason to believe that the storage
  // requirements (nk) will always grow linearly like this, but I
  // don't really anticipate adding any other schemes soon anyway, so
  // the fact that this works well for the two we have is enough.
  size_t nk = ret->order + 2;
  ret->k = R_Calloc(nk, double*);
  for (size_t i = 0; i < nk; ++i) {
    ret->k[i] = R_Calloc(n, double);
  }

  ret->history_len = 2 + ret->order * n;
  ret->history =
    ring_buffer_create(n_history, ret->history_len * sizeof(double));
  ret->history_time_idx = ret->order * n;

  // Defaults!
  // TODO: Support vectorised tolerances?
  ret->atol = 1e-6;
  ret->rtol = 1e-6;

  // TODO: These all need to be modifiable from R
  switch (ret->method) {
  case DOPRI_5:
    // TODO: these are different for dde; switch again on delay and document.
    ret->step_factor_min = 0.2;  // from dopri5.f:276
    ret->step_factor_max = 10.0; // from dopri5.f:281
    ret->step_beta = 0.04;       // from dopri5.f:287
    break;
  case DOPRI_853:
    ret->step_factor_min = 0.333; // from dopri853.f:285
    ret->step_factor_max = 6.0;   // from dopri853.f:290
    ret->step_beta = 0.0;         // from dopri853.f:296
    break;
  }
  ret->step_size_max = DBL_MAX;
  ret->step_size_initial = 0.0;
  ret->step_max_n = 100000;    // from dopri5.f:212
  ret->step_factor_safe = 0.9; // from dopri5.f:265

  return ret;
}

// We'll need a different reset when we're providing history, because
// then we won't end up resetting t0/y0 the same way.
void dopri_data_reset(dopri_data *obj, double *y,
                       double *times, size_t n_times,
                       double *tcrit, size_t n_tcrit) {
  obj->error = false;
  obj->code = NOT_SET;
  memcpy(obj->y0, y, obj->n * sizeof(double));
  memcpy(obj->y, y, obj->n * sizeof(double));
  obj->t0 = times[0];
  obj->t = times[0];
  if (obj->n_times != n_times) {
    R_Free(obj->times); // consider realloc?
    obj->times = R_Calloc(n_times, double);
    obj->n_times = n_times;
  }
  memcpy(obj->times, times, n_times * sizeof(double));
  obj->times_idx = 1; // skipping the first time!

  if (obj->n_tcrit != n_tcrit) {
    R_Free(obj->tcrit); // consider realloc?
    if (n_tcrit > 0) {
      obj->tcrit = R_Calloc(n_tcrit, double);
    } else {
      obj->tcrit = NULL;
    }
    obj->n_tcrit = n_tcrit;
  }
  memcpy(obj->tcrit, tcrit, n_tcrit * sizeof(double));
  obj->tcrit_idx = 0;
  if (n_tcrit > 0) {
    while (tcrit[obj->tcrit_idx] < obj->t0 && obj->tcrit_idx < n_tcrit) {
      obj->tcrit_idx++;
    }
  }

  obj->sign = copysign(1.0, times[1] - times[0]);
  obj->n_eval = 0;
  obj->n_step = 0;
  obj->n_accept = 0;
  obj->n_reject = 0;
}

void dopri_data_free(dopri_data *obj) {
  R_Free(obj->y0);
  R_Free(obj->y);
  R_Free(obj->y1);

  size_t nk = obj->order + 2;
  for (size_t i = 0; i < nk; ++i) {
    R_Free(obj->k[i]);
  }
  R_Free(obj->k);

  ring_buffer_destroy(obj->history);

  R_Free(obj->times);
  R_Free(obj);
}

// This is super ugly, but needs to be done so that the lag functions
// can access the previous history easily.  I don't see an obvious way
// around this unfortunately, given that the lag functions need to be
// callable in user code (so without forcing some weird and blind
// passing of a void object around, which will break compatibility
// with deSolve even more and make the interface for the dde and
// non-dde equations quite different) this seems like a reasonable way
// of achiving this.  Might change later though.
static dopri_data *dde_global_obj;

// Used to query the problem size safely from the interface.c file
size_t get_current_problem_size() {
  return dde_global_obj == NULL ? 0 : dde_global_obj->n;
}

// Wrappers around the two methods:
void dopri_step(dopri_data *obj, double h) {
  switch (obj->method) {
  case DOPRI_5:
    dopri5_step(obj, h);
    break;
  case DOPRI_853:
    Rf_error("not yet implemented");
    break;
  }
}

double dopri_error(dopri_data *obj) {
  switch (obj->method) {
  case DOPRI_5:
    return dopri5_error(obj);
  case DOPRI_853:
    Rf_error("not yet implemented");
    return 0;
  }
}

void dopri_save_history(dopri_data *obj, double h) {
  switch (obj->method) {
  case DOPRI_5:
    dopri5_save_history(obj, h);
    break;
  case DOPRI_853:
    Rf_error("not yet implemented");
    break;
  }
}

// Integration is going to be over a set of times 't', of which there
// are 'n_t'.
void dopri_integrate(dopri_data *obj, double *y,
                      double *times, size_t n_times,
                      double *tcrit, size_t n_tcrit,
                      double *y_out, double *out) {
  // TODO: check that t is strictly sorted and n_times >= 2 (in R)
  dopri_data_reset(obj, y, times, n_times, tcrit, n_tcrit);

  double fac_old = 1e-4;
  double uround = 10 * DBL_EPSILON;
  bool stop = false, last = false, reject = false;

  double t_end = times[n_times - 1];
  double t_stop = t_end;
  if (obj->tcrit_idx < obj->n_tcrit &&
      obj->tcrit[obj->tcrit_idx] < t_end) {
    t_stop = obj->tcrit[obj->tcrit_idx];
  } else {
    t_stop = t_end;
  }

  // Possibly only set this if the number of history variables is
  // nonzero?  Needs to be set before any calls to target() though.
  dde_global_obj = obj;

  obj->target(obj->n, obj->t, obj->y, obj->k[0], obj->data);
  obj->n_eval++;

  // Work out the initial step size:
  double h = dopri_h_init(obj);
  double h_save = 0.0;

  while (true) {
    if (obj->n_step > obj->step_max_n) {
      obj->error = true;
      obj->code = ERR_TOO_MANY_STEPS;
      break;
    }
    if (0.1 * fabs(h) <= fabs(obj->t) * uround) {
      obj->error = true;
      obj->code = ERR_STEP_SIZE_TOO_SMALL;
      break;
    }
    if ((obj->t + 1.01 * h - t_end) * obj->sign > 0.0) {
      h_save = h;
      h = t_end - obj->t;
      last = true;
    } else if ((obj->t + 1.01 * h - t_stop) * obj->sign > 0.0) {
      h = t_stop - obj->t;
      stop = true;
    }
    obj->n_step++;

    // TODO: In the Fortran there is an option here to check the irtrn
    // flag for a code of '2' which indicates that the variables need
    // recomputing.  This would be the case possibly where output
    // variables are being calculated so might want to put this back
    // in once the signalling is done.

    dopri_step(obj, h);

    // Error estimation:
    double err = dopri_error(obj);
    double h_new = dopri_h_new(obj, fac_old, h, err);

    if (err <= 1) {
      // Step is accepted :)
      fac_old = fmax(err, 1e-4);
      obj->n_accept++;
      // TODO: Stiffness detection, once done.
      dopri_save_history(obj, h);

      // TODO: it's quite possible that we can swap the pointers here
      // and avoid the memcpy.
      switch (obj->method) {
      case DOPRI_5:
        memcpy(obj->k[0], obj->k[1], obj->n * sizeof(double)); // k1 = k2
        memcpy(obj->y,    obj->y1,   obj->n * sizeof(double)); // y  = y1
        break;
      case DOPRI_853:
        memcpy(obj->k[0], obj->k[3], obj->n * sizeof(double)); // k1 = k4
        memcpy(obj->y,    obj->k[4], obj->n * sizeof(double)); // y  = k5
        break;
      }
      obj->t += h;

      while (obj->times_idx < obj->n_times &&
             obj->times[obj->times_idx] <= obj->t) {
        // Here, it might be nice to allow transposed output or not;
        // that would be an argument to interpolate_all.  That's a bit
        // of a faff.
        dopri_interpolate_all((double *) obj->history->head, obj->method,
                               obj->n, obj->times[obj->times_idx], y_out);
        if (obj->n_out > 0) {
          obj->output(obj->n, obj->times[obj->times_idx], y_out,
                      obj->n_out, out, obj->data);
          out += obj->n_out;
        }

        y_out += obj->n;
        obj->times_idx++;
      }

      // Advance the ring buffer; we'll write to the next place after
      // this.
      ring_buffer_head_advance(obj->history);

      if (last) {
        obj->step_size_initial = h_save;
        obj->code = OK_COMPLETE;
        break;
      }
      // TODO: To understand this bit I think I will need to get the
      // book and actually look at the dopri integration bit.
      if (fabs(h_new) >= obj->step_size_max) {
        h_new = copysign(obj->step_size_max, obj->sign);
      }
      if (reject) {
        h_new = copysign(fmin(fabs(h_new), fabs(h)), obj->sign);
        reject = false;
      }
      if (stop) {
        obj->tcrit_idx++;
        if (obj->tcrit_idx < obj->n_tcrit &&
            obj->tcrit[obj->tcrit_idx] < t_end) {
          t_stop = obj->tcrit[obj->tcrit_idx];
        } else {
          t_stop = t_end;
        }
        stop = false;
      } else {
        h = h_new;
      }
    } else {
      // Step is rejected :(
      //
      // TODO: This is very annoying because we need to recompute
      // fac11 here, and re-invert the minimum step factor.
      //
      // TODO: move this thing (arg2 of pow) into the struct?
      double fac11 = pow(err, 0.2 - obj->step_beta * 0.75);
      h_new = h / fmin(1 / obj->step_factor_min, fac11 / obj->step_factor_safe);
      reject = true;
      if (obj->n_accept >= 1) {
        obj->n_reject++;
      }
      last = false;
      stop = false;
      h = h_new;
    }
  }

  // Reset the global state
  dde_global_obj = NULL;
}

// Shared
double dopri_h_new(dopri_data *obj, double fac_old, double h, double err) {
  double expo1 = 0.2 - obj->step_beta * 0.75;
  double fac11 = pow(err, expo1);
  double step_factor_min = 1.0 / obj->step_factor_min;
  double step_factor_max = 1.0 / obj->step_factor_max;
  // Lund-stabilisation
  double fac = fac11 / pow(fac_old, obj->step_beta);
  fac = fmax(step_factor_max,
             fmin(step_factor_min, fac / obj->step_factor_safe));
  return h / fac;
}

double dopri_h_init(dopri_data *obj) {
  if (obj->step_size_initial > 0.0) {
    return obj->step_size_initial;
  }

  // NOTE: This is destructive with respect to most of the information
  // in the object; in particular k2, k3 will be modified.
  double *f0 = obj->k[0], *f1 = obj->k[1], *y1 = obj->k[2];

  // Compute a first guess for explicit Euler as
  //   h = 0.01 * norm (y0) / norm (f0)
  // the increment for explicit euler is small compared to the solution
  double norm_f = 0.0, norm_y = 0.0;
  for (size_t i = 0; i < obj->n; ++i) {
    double sk = obj->atol + obj->rtol * fabs(obj->y[i]);
    norm_f += square(f0[i] / sk);
    norm_y += square(obj->y[i]  / sk);
  }
  double h = (norm_f <= 1e-10 || norm_f <= 1e-10) ?
    1e-6 : sqrt(norm_y / norm_f) * 0.01;
  h = copysign(fmin(h, obj->step_size_max), obj->sign);

  // Perform an explicit Euler step
  for (size_t i = 0; i < obj->n; ++i) {
    y1[i] = obj->y[i] + h * f0[i];
  }
  obj->target(obj->n, obj->t + h, y1, f1, obj->data);
  obj->n_eval++;

  // Estimate the second derivative of the solution:
  double der2 = 0.0;
  for (size_t i = 0; i < obj->n; ++i) {
    double sk = obj->atol + obj->rtol * fabs(obj->y[i]);
    der2 += square((f1[i] - f0[i]) / sk);
  }
  der2 = sqrt(der2) / h;

  // Step size is computed such that
  //   h^order * fmax(norm(f0), norm(der2)) = 0.01
  double der12 = fmax(fabs(der2), sqrt(norm_f));
  double h1 = (der12 <= 1e-15) ?
    fmax(1e-6, fabs(h) * 1e-3) : pow(0.01 / der12, 1.0 / obj->order);
  h = fmin(fmin(100 * fabs(h), h1), obj->step_size_max);
  return copysign(h, obj->sign);
}

// Specific...

// There are several interpolation functions here;
//
// * interpolate_1; interpolate a single variable i
// * interpolate_all: interpolate the entire vector
// * interpolate_idx: interpolate some of the vector
// * interpolate_idx_int: As for _idx but with an integer index (see below)
double dopri_interpolate_1(const double *history, dopri_method method,
                           size_t n, double t, size_t i) {
  const size_t idx_t = (method == DOPRI_5 ? 5 : 8) * n;
  const double t_old = history[idx_t], h = history[idx_t + 1];
  const double theta = (t - t_old) / h;
  const double theta1 = 1 - theta;
  switch (method) {
  case DOPRI_5:
    return dopri5_interpolate(n, theta, theta1, history + i);
    break;
  case DOPRI_853:
    Rf_error("not yet implemented");
    return 0; // not implemented!
    break;
  }
}

void dopri_interpolate_all(const double *history, dopri_method method,
                           size_t n, double t, double *y) {
  const size_t idx_t = (method == DOPRI_5 ? 5 : 8) * n;
  const double t_old = history[idx_t], h = history[idx_t + 1];
  const double theta = (t - t_old) / h;
  const double theta1 = 1 - theta;
  switch (method) {
  case DOPRI_5:
    for (size_t i = 0; i < n; ++i) {
      y[i] = dopri5_interpolate(n, theta, theta1, history + i);
    }
    break;
  case DOPRI_853:
    Rf_error("not yet implemented");
    break;
  }
}

void dopri_interpolate_idx(const double *history, dopri_method method,
                           size_t n, double t, size_t * idx, size_t nidx,
                           double *y) {
  const size_t idx_t = (method == DOPRI_5 ? 5 : 8) * n;
  const double t_old = history[idx_t], h = history[idx_t + 1];
  const double theta = (t - t_old) / h;
  const double theta1 = 1 - theta;
  switch (method) {
  case DOPRI_5:
    for (size_t i = 0; i < nidx; ++i) {
      y[i] = dopri5_interpolate(n, theta, theta1, history + idx[i]);
    }
    break;
  case DOPRI_853:
    Rf_error("not yet implemented");
    break;
  }
}

// This exists to deal with deSolve taking integer arguments (and
// therefore messing up odin).  I could rewrite the whole thing here
// to use int, but that seems needlessly crap.  The issue here is only
// the *pointer* '*idx' and not anything else because we can safely
// cast the plain data arguments.  This affects only this function as
// it's the only one that takes size_t*
void dopri_interpolate_idx_int(const double *history, dopri_method method,
                               size_t n, double t, int *idx, size_t nidx,
                               double *y) {
  const size_t idx_t = (method == DOPRI_5 ? 5 : 8) * n;
  const double t_old = history[idx_t], h = history[idx_t + 1];
  const double theta = (t - t_old) / h;
  const double theta1 = 1 - theta;
  switch (method) {
  case DOPRI_5:
    for (size_t i = 0; i < nidx; ++i) {
      y[i] = dopri5_interpolate(n, theta, theta1, history + idx[i]);
    }
    break;
  case DOPRI_853:
    Rf_error("not yet implemented");
    break;
  }
}

// history searching:
//
// The big challenge here is that the history object will need to be
// found.  deSolve deals with this generally by stashing a copy of the
// objects as a global, This is nice because then the target function
// does not need to know about the struct; if you require that the
// struct is present then the whole thing falls apart a little bit
// because the target function is in the main struct, but needs to
// *call* the main struct to get the history.  So, this is a bit
// tricky to get right.

// The global/lock solver approach of deSolve is reasonable, and while
// inelegant should serve us reasonably well.  The other approach
// would be to somehow do a more C++ approach where things know more
// about the solution but that seems shit.

// So I'll need to get this right.  On integration we'll have a
// `global_state` variable that is (possibly static) initialised when
// the integration starts, and which holds a pointer to the
// integration data.  Then when the integration runs any call to ylag*
// will end up triggering that.  On exit of the itegrator we'll set
// that to NULL and hope that there are no longjup style exists
// anywhere so we can use `==NULL` safely.

// TODO: There's an issue here where if the chosen time is the last
// time (a tail offset of n-1) then we might actually need to
// interpolate off of the head which is not actually returned.  This
// is a bit of a shit, and might be worth treating separately in the
// search, or in the code below.

// These bits are all nice and don't use any globals
struct dopri_find_time_pred_data {
  size_t idx;
  double time;
};

bool dopri_find_time_pred(const void *x, void *data) {
  const struct dopri_find_time_pred_data *d =
    (struct dopri_find_time_pred_data *) data;
  return ((double*) x)[d->idx] <= d->time;
}

const double* dopri_find_time(dopri_data *obj, double t) {
  const size_t idx_t = obj->history_time_idx;
  struct dopri_find_time_pred_data data = {idx_t, t};
  // The first shot at idx here is based on a linear interpolation of
  // the time; hopefully this gets is close to the correct point
  // without having to have a really long search time.
  const size_t n = ring_buffer_used(obj->history, 0);
  size_t idx0;
  if (n > 0) {
    const double
      t0 = ((double*) ring_buffer_tail(obj->history))[idx_t],
      t1 = ((double*) ring_buffer_tail_offset(obj->history, n - 1))[idx_t];
    idx0 = (t1 - t0) / (n - 1);
  } else {
    idx0 = 0;
  }
  const void *h = ring_buffer_search_bisect(obj->history, idx0,
                                            &dopri_find_time_pred,
                                            &data);
  if (h == NULL) {
    Rf_error("Cannot find time within buffer");
  }
  return (double*) h;
}

// But these all use the global state object (otherwise these all pick
// up a `void *data` argument which will be cast to `dopri_data*`,
// but then the derivative function needs the same thing, which is
// going to seem weird and also means that the same function can't be
// easily used for dde and non dde use).
double ylag_1(double t, size_t i) {
  if (t <= dde_global_obj->t0) {
    return dde_global_obj->y0[i];
  } else {
    const double * h = dopri_find_time(dde_global_obj, t);
    return dopri_interpolate_1(h, dde_global_obj->method,
                               dde_global_obj->n, t, i);
  }
}

void ylag_all(double t, double *y) {
  if (t <= dde_global_obj->t0) {
    memcpy(y, dde_global_obj->y0, dde_global_obj->n * sizeof(double));
  } else {
    const double * h = dopri_find_time(dde_global_obj, t);
    dopri_interpolate_all(h, dde_global_obj->method, dde_global_obj->n, t, y);
  }
}

void ylag_vec(double t, size_t *idx, size_t nidx, double *y) {
  if (t <= dde_global_obj->t0) {
    for (size_t i = 0; i < nidx; ++i) {
      y[i] = dde_global_obj->y0[idx[i]];
    }
  } else {
    const double * h = dopri_find_time(dde_global_obj, t);
    dopri_interpolate_idx(h, dde_global_obj->method,
                          dde_global_obj->n, t, idx, nidx, y);
  }
}

void ylag_vec_int(double t, int *idx, size_t nidx, double *y) {
  if (t <= dde_global_obj->t0) {
    for (size_t i = 0; i < nidx; ++i) {
      y[i] = dde_global_obj->y0[idx[i]];
    }
  } else {
    const double * h = dopri_find_time(dde_global_obj, t);
    dopri_interpolate_idx_int(h, dde_global_obj->method,
                              dde_global_obj->n, t, idx, nidx, y);
  }
}

// Utility
double square(double x) {
  return x * x;
}