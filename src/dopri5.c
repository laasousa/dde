#include "dopri5.h"
#include "dopri5_constants.h"
#include "util.h"
#include <R.h>

dopri5_data* dopri5_data_alloc(deriv_func* target, size_t n, void *data) {
  dopri5_data *ret = (dopri5_data*) R_Calloc(1, dopri5_data);
  ret->target = target;
  ret->data = data;

  ret->n = n;
  ret->initialised = false;

  ret->n_times = 0;
  ret->times = NULL;

  ret->y0 = R_Calloc(n, double);
  ret->y  = R_Calloc(n, double);
  ret->y1 = R_Calloc(n, double);

  ret->k1 = R_Calloc(n, double);
  ret->k2 = R_Calloc(n, double);
  ret->k3 = R_Calloc(n, double);
  ret->k4 = R_Calloc(n, double);
  ret->k5 = R_Calloc(n, double);
  ret->k6 = R_Calloc(n, double);
  ret->ysti = R_Calloc(n, double);

  ret->history_len = 2 + 5 * n;
  size_t n_history = 100;
  ret->history =
    ring_buffer_create(n_history, ret->history_len * sizeof(double));

  // Defaults!
  ret->atol = 1e-6;
  ret->rtol = 1e-6;

  // NOTE: these are different for dde!
  ret->step_factor_min = 0.2;  // from dopri5.f:276
  ret->step_factor_max = 10.0; // from dopri5.f:281
  ret->step_size_max = DBL_MAX;
  ret->step_size_initial = 0.0;
  ret->step_max_n = 100000;    // from dopri5.f:212
  // NOTE: beta is different for dde and dopri5
  ret->step_beta = 0.04;       // from dopri5.f:287
  ret->step_factor_safe = 0.9; // from dopri5.f:265

  return ret;
}

void dopri5_data_reset(dopri5_data *obj, double *y,
                       double *times, size_t n_times) {
  memcpy(obj->y0, y, obj->n * sizeof(double));
  memcpy(obj->y, y, obj->n * sizeof(double));
  obj->t0 = times[0];
  obj->t = times[0];
  if (obj->n_times != n_times) {
    // consider realloc?
    R_Free(obj->times);
    obj->times = R_Calloc(n_times, double);
    obj->n_times = n_times;
  }
  memcpy(obj->times, times, n_times * sizeof(double));
  obj->times_idx = 1; // skipping the first time!

  obj->sign = copysign(1.0, times[1] - times[0]);
  obj->n_eval = 0;
  obj->n_step = 0;
  obj->n_accept = 0;
  obj->n_reject = 0;
  obj->initialised = true;
}

void dopri5_data_free(dopri5_data *obj) {
  // TODO: Don't use initialised here; this is currently a potential
  // leak and not correct.
  if (obj->initialised) {
    R_Free(obj->y0);
    R_Free(obj->y);
    R_Free(obj->y1);
    R_Free(obj->k2);
    R_Free(obj->k3);
    R_Free(obj->k4);
    R_Free(obj->k5);
    R_Free(obj->k6);
    R_Free(obj->ysti);
    ring_buffer_destroy(obj->history);
  }
  R_Free(obj->times);
  R_Free(obj);
}

// Integration is going to be over a set of times 't', of which there
// are 'n_t'.
void dopri5_integrate(dopri5_data *obj, double *y,
                      double *times, size_t n_times,
                      double *y_out) {
  obj->error = false;
  obj->code = NOT_SET;

  // TODO: check that t is strictly sorted and n_times >= 2
  dopri5_data_reset(obj, y, times, n_times);

  double fac_old = 1e-4;
  double uround = 10 * DBL_EPSILON;
  bool last = false, reject = false;

  double t_end = times[n_times - 1];

  obj->target(obj->n, obj->t, obj->y, obj->k1, obj->data);
  obj->n_eval++;

  // Work out the initial step size:
  double h = dopri5_h_init(obj);

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
      h = t_end - obj->t;
      last = true;
    }
    obj->n_step++;

    // TODO: In the Fortran there is an option here to check the irtrn
    // flag for a code of '2' which indicates that the variables need
    // recomputing.  This would be the case possibly where output
    // variables are being calculated so might want to put this back
    // in once the signalling is done.

    dopri5_step(obj, h);

    // Error estimation:
    double err = dopri5_error(obj);
    double h_new = dopri5_h_new(obj, fac_old, h, err);

    if (err <= 1) {
      // Step is accepted :)
      fac_old = fmax(err, 1e-4);
      obj->n_accept++;
      // TODO: Stiffness detection, once done.
      // TODO: make conditional
      double *history = (double*) obj->history->head;
      for (size_t i = 0; i < obj->n; ++i) {
        double ydiff = obj->y1[i] - obj->y[i];
        double bspl = h * obj->k1[i] - ydiff;
        history[             i] = obj->y[i];
        history[    obj->n + i] = ydiff;
        history[2 * obj->n + i] = bspl;
        history[3 * obj->n + i] = -h * obj->k2[i] + ydiff - bspl;
      }
      history[5 * obj->n    ] = obj->t;
      history[5 * obj->n + 1] = h;

      // Always do these bits (TODO, it's quite possible that we can
      // do this with a swap rather than a memcpy which would be
      // nice).
      memcpy(obj->k1, obj->k2, obj->n * sizeof(double));
      memcpy(obj->y,  obj->y1, obj->n * sizeof(double));
      obj->t += h;

      while (obj->times_idx < obj->n_times &&
             obj->times[obj->times_idx] <= obj->t) {
        dopri5_interpolate(obj, obj->times[obj->times_idx], y_out);
        obj->times_idx++;
        y_out += obj->n;
      }
      if (last) {
        // TODO: we could save h back into obj here?
        obj->code = OK_COMPLETE;
        return;
      }
      // Advance the ring buffer; we'll write to the next place after
      // this.
      ring_buffer_head_advance(obj->history);
      // TODO: To understand this bit I think I will need to get the
      // book and actually look at the dopri integration bit.
      if (fabs(h_new) >= obj->step_size_max) {
        h_new = copysign(obj->step_size_max, obj->sign);
      }
      if (reject) {
        h_new = copysign(fmin(fabs(h_new), fabs(h)), obj->sign);
        reject = false;
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
    }
    h = h_new;
  }
}

void dopri5_step(dopri5_data *obj, double h) {
  const double t = obj->t;
  const size_t n = obj->n;
  for (size_t i = 0; i < n; ++i) { // 22
    obj->y1[i] = obj->y[i] + h * A21 * obj->k1[i];
  }
  obj->target(n, t + C2 * h, obj->y1, obj->k2, obj->data);
  for (size_t i = 0; i < n; ++i) { // 23
    obj->y1[i] = obj->y[i] + h * (A31 * obj->k1[i] + A32 * obj->k2[i]);
  }
  obj->target(n, t + C3 * h, obj->y1, obj->k3, obj->data);
  for (size_t i = 0; i < n; ++i) { // 24
    obj->y1[i] = obj->y[i] + h * (A41 * obj->k1[i] + A42 * obj->k2[i] +
                                  A43 * obj->k3[i]);
  }
  obj->target(n, t + C4 * h, obj->y1, obj->k4, obj->data);
  for (size_t i = 0; i < n; ++i) { // 25
    obj->y1[i] = obj->y[i] + h * (A51 * obj->k1[i] + A52 * obj->k2[i] +
                                  A53 * obj->k3[i] + A54 * obj->k4[i]);
  }
  obj->target(n, t + C5 * h, obj->y1, obj->k5, obj->data);
  for (size_t i = 0; i < n; ++i) { // 26
    obj->ysti[i] = obj->y[i] + h * (A61 * obj->k1[i] + A62 * obj->k2[i] +
                                    A63 * obj->k3[i] + A64 * obj->k4[i] +
                                    A65 * obj->k5[i]);
  }
  double t_next = t + h;
  obj->target(n, t_next, obj->ysti, obj->k6, obj->data);
  for (size_t i = 0; i < n; ++i) { // 27
    obj->y1[i] = obj->y[i] + h * (A71 * obj->k1[i] + A73 * obj->k3[i] +
                                  A74 * obj->k4[i] + A75 * obj->k5[i] +
                                  A76 * obj->k6[i]);
  }
  obj->target(n, t_next, obj->y1, obj->k2, obj->data);

  // TODO: Doing this unconditionally at the moment, but this should
  // be tuned, and possibly thinned (e.g., with the index thing).
  double *history = (double*) obj->history->head;
  for (size_t i = 0, j = 4 * n; i < n; ++i, ++j) {
    history[j] =
      h * (D1 * obj->k1[i] + D3 * obj->k3[i] + D4 * obj->k4[i] +
           D5 * obj->k5[i] + D6 * obj->k6[i] + D7 * obj->k2[i]);
  }

  for (size_t i = 0; i < n; ++i) {
    obj->k4[i] = h * (E1 * obj->k1[i] + E3 * obj->k3[i] + E4 * obj->k4[i] +
                      E5 * obj->k5[i] + E6 * obj->k6[i] + E7 * obj->k2[i]);
  }
}

double dopri5_error(dopri5_data *obj) {
  double err = 0.0;
  for (size_t i = 0; i < obj->n; ++i) {
    double sk = obj->atol + obj->rtol * fmax(fabs(obj->y[i]), fabs(obj->y1[i]));
    err += square(obj->k4[i] / sk);
  }
  return sqrt(err / obj->n);
}

double dopri5_h_new(dopri5_data *obj, double fac_old, double h, double err) {
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

double dopri5_h_init(dopri5_data *obj) {
  if (obj->step_size_initial > 0.0) {
    return obj->step_size_initial;
  }

  // NOTE: This is destructive with respect to most of the information
  // in the object; in particular k2, k3 will be modified.
  double *f0 = obj->k1, *f1 = obj->k2, *y1 = obj->k3;

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
  //   h^iord * fmax(norm(f0), norm(der2)) = 0.01
  int iord = 5;
  double der12 = fmax(fabs(der2), sqrt(norm_f));
  double h1 = (der12 <= 1e-15) ?
    fmax(1e-6, fabs(h) * 1e-3) : pow(0.01 / der12, 1.0 / iord);
  h = fmin(fmin(100 * fabs(h), h1), obj->step_size_max);
  return copysign(h, obj->sign);
}

// There are two interpolation functions here; one (interpolate1())
// interpolates a single variable while the other (interpolate())
// interpolates the entire y vector.
double dopri5_interpolate1(dopri5_data *obj, double t, size_t i) {
  double *history = (double*) obj->history->head;
  const size_t n = obj->n;
  const double t_old = history[5 * n], h = history[5 * n + 1];
  const double theta = (t - t_old) / h;
  const double theta1 = 1 - theta;

  return history[i] + theta *
    (history[n + i] + theta1 *
     (history[2 * n + i] + theta *
      (history[3 * n + i] + theta1 *
       history[4 * n + i])));
}

void dopri5_interpolate(dopri5_data *obj, double t, double *y) {
  double *history = (double*) obj->history->head;
  const size_t n = obj->n;
  const double t_old = history[5 * n], h = history[5 * n + 1];
  const double theta = (t - t_old) / h;
  const double theta1 = 1 - theta;

  for (size_t i = 0; i < obj->n; ++i) {
    y[i] = history[i] + theta *
      (history[n + i] + theta1 *
       (history[2 * n + i] + theta *
        (history[3 * n + i] + theta1 *
         history[4 * n + i])));
  }
}
