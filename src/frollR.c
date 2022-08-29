#include "data.table.h"  // first (before Rdefines.h) for clang-13-omp, #5122
#include <Rdefines.h>

SEXP coerceToRealListR(SEXP obj) {
  // accept atomic/list of integer/logical/real returns list of real
  int protecti = 0;
  if (isVectorAtomic(obj)) {
    SEXP obj1 = obj;
    obj = PROTECT(allocVector(VECSXP, 1)); protecti++;
    SET_VECTOR_ELT(obj, 0, obj1);
  }
  R_len_t nobj = length(obj);
  SEXP x = PROTECT(allocVector(VECSXP, nobj)); protecti++;
  for (R_len_t i=0; i<nobj; i++) {
    SEXP this_obj = VECTOR_ELT(obj, i);
    if (!(isReal(this_obj) || isInteger(this_obj) || isLogical(this_obj)))
      error(_("x must be of type numeric or logical, or a list, data.frame or data.table of such"));
    SET_VECTOR_ELT(x, i, coerceAs(this_obj, ScalarReal(NA_REAL), /*copyArg=*/ScalarLogical(false))); // copyArg=false will make type-class match to return as-is, no copy
  }
  UNPROTECT(protecti);
  return x;
}

SEXP frollfunR(SEXP fun, SEXP obj, SEXP k, SEXP fill, SEXP algo, SEXP align, SEXP narm, SEXP hasnf, SEXP adaptive) {
  int protecti = 0;
  const bool verbose = GetVerbose();

  if (!xlength(obj))
    return(obj);                                                // empty input: NULL, list()
  double tic = 0;
  if (verbose)
    tic = omp_get_wtime();
  SEXP x = PROTECT(coerceToRealListR(obj)); protecti++;
  R_len_t nx=length(x);                                         // number of columns to roll on

  if (xlength(k) == 0)                                          // check that window is non zero length
    error(_("n must be non 0 length"));

  if (!IS_TRUE_OR_FALSE(adaptive))
    error(_("%s must be TRUE or FALSE"), "adaptive");
  bool badaptive = LOGICAL(adaptive)[0];

  R_len_t nk = 0;                                               // number of rolling windows, for adaptive might be atomic to be wrapped into list, 0 for clang -Wall
  SEXP ik = R_NilValue;                                         // holds integer window width, if doing non-adaptive roll fun
  SEXP kl = R_NilValue;                                         // holds adaptive window width, if doing adaptive roll fun
  if (!badaptive) {                                             // validating n input for adaptive=FALSE
    if (isNewList(k))
      error(_("n must be integer, list is accepted for adaptive TRUE"));

    if (isInteger(k)) {                                        // check that k is integer vector
      ik = k;
    } else if (isReal(k)) {                                    // if n is double then convert to integer
      ik = PROTECT(coerceVector(k, INTSXP)); protecti++;
    } else {
      error(_("n must be integer"));
    }

    nk = length(k);
    R_len_t i=0;                                                // check that all window values positive
    while (i < nk && INTEGER(ik)[i] > 0) i++;
    if (i != nk)
      error(_("n must be positive integer values (> 0)"));
  } else {                                                      // validating n input for adaptive=TRUE
    if (isVectorAtomic(k)) {                                    // if not-list then wrap into list
      kl = PROTECT(allocVector(VECSXP, 1)); protecti++;
      if (isInteger(k)) {                                       // check that k is integer vector
        SET_VECTOR_ELT(kl, 0, k);
      } else if (isReal(k)) {                                   // if n is double then convert to integer
        SET_VECTOR_ELT(kl, 0, coerceVector(k, INTSXP));
      } else {
        error(_("n must be integer vector or list of integer vectors"));
      }
      nk = 1;
    } else {
      nk = length(k);
      kl = PROTECT(allocVector(VECSXP, nk)); protecti++;
      for (R_len_t i=0; i<nk; i++) {
        if (isInteger(VECTOR_ELT(k, i))) {
          SET_VECTOR_ELT(kl, i, VECTOR_ELT(k, i));
        } else if (isReal(VECTOR_ELT(k, i))) {                  // coerce double types to integer
          SET_VECTOR_ELT(kl, i, coerceVector(VECTOR_ELT(k, i), INTSXP));
        } else {
          error(_("n must be integer vector or list of integer vectors"));
        }
      }
    }
  }
  int **ikl = (int**)R_alloc(nk, sizeof(int*));                 // to not recalculate `length(x[[i]])` we store it in extra array
  if (badaptive) {
    for (int j=0; j<nk; j++)
      ikl[j] = INTEGER(VECTOR_ELT(kl, j));
  }
  int* iik = NULL;
  if (!badaptive) {
    if (!isInteger(ik))
      error(_("Internal error: badaptive=%d but ik is not integer"), badaptive); // # nocov
    iik = INTEGER(ik);                                          // pointer to non-adaptive window width, still can be vector when doing multiple windows
  } else {
    // ik is still R_NilValue from initialization. But that's ok as it's only needed below when !badaptive.
  }

  if (!IS_TRUE_OR_FALSE(narm))
    error(_("%s must be TRUE or FALSE"), "na.rm");

  if (!isLogical(hasnf) || length(hasnf)!=1)
    error(_("has.nf must be TRUE, FALSE or NA"));
  if (LOGICAL(hasnf)[0]==FALSE && LOGICAL(narm)[0])
    error(_("using has.nf FALSE and na.rm TRUE does not make sense, if you know there are non-finite values then use has.nf TRUE, otherwise leave it as default NA"));

  int ialign;                                                   // decode align to integer
  if (!strcmp(CHAR(STRING_ELT(align, 0)), "right"))
    ialign = 1;
  else if (!strcmp(CHAR(STRING_ELT(align, 0)), "center"))
    ialign = 0;
  else if (!strcmp(CHAR(STRING_ELT(align, 0)), "left"))
    ialign = -1;
  else
    error(_("Internal error: invalid %s argument in %s function should have been caught earlier. Please report to the data.table issue tracker."), "align", "rolling"); // # nocov

  if (badaptive && ialign==0) // support for left added in #5441
    error(_("using adaptive TRUE and align 'center' is not implemented"));

  SEXP ans = PROTECT(allocVector(VECSXP, nk * nx)); protecti++; // allocate list to keep results
  if (verbose)
    Rprintf(_("%s: allocating memory for results %dx%d\n"), __func__, nx, nk);
  ans_t *dans = (ans_t *)R_alloc(nx*nk, sizeof(ans_t));         // answer columns as array of ans_t struct
  double** dx = (double**)R_alloc(nx, sizeof(double*));         // pointers to source columns
  uint64_t* inx = (uint64_t*)R_alloc(nx, sizeof(uint64_t));     // to not recalculate `length(x[[i]])` we store it in extra array
  for (R_len_t i=0; i<nx; i++) {
    inx[i] = xlength(VECTOR_ELT(x, i));                         // for list input each vector can have different length
    for (R_len_t j=0; j<nk; j++) {
      if (badaptive) {                                          // extra input validation
        if (i > 0 && (inx[i]!=inx[i-1]))                        // variable length list input not allowed for adaptive roll
          error(_("adaptive rolling function can only process 'x' having equal length of elements, like data.table or data.frame; If you want to call rolling function on list having variable length of elements call it for each field separately"));
        if (xlength(VECTOR_ELT(kl, j))!=inx[0])                 // check that length of integer vectors in n list match to xrows[0] ([0] and not [i] because there is above check for equal xrows)
          error(_("length of integer vector(s) provided as list to 'n' argument must be equal to number of observations provided in 'x'"));
      }
      SET_VECTOR_ELT(ans, i*nk+j, allocVector(REALSXP, inx[i]));// allocate answer vector for this column-window
      dans[i*nk+j] = ((ans_t) { .dbl_v=REAL(VECTOR_ELT(ans, i*nk+j)), .status=0, .message={"\0","\0","\0","\0"} });
    }
    dx[i] = REAL(VECTOR_ELT(x, i));                             // assign source columns to C pointers
  }

  rollfun_t rfun; // adding fun needs to be here and data.table.h
  if (!strcmp(CHAR(STRING_ELT(fun, 0)), "mean")) {
    rfun = MEAN;
  } else if (!strcmp(CHAR(STRING_ELT(fun, 0)), "sum")) {
    rfun = SUM;
  } else if (!strcmp(CHAR(STRING_ELT(fun, 0)), "max")) {
    rfun = MAX;
  } else {
    error(_("Internal error: invalid %s argument in %s function should have been caught earlier. Please report to the data.table issue tracker."), "fun", "rolling"); // # nocov
  }

  if (length(fill) != 1)
    error(_("fill must be a vector of length 1"));
  if (!isInteger(fill) && !isReal(fill) && !isLogical(fill))
    error(_("fill must be numeric or logical"));
  double dfill = REAL(PROTECT(coerceAs(fill, ScalarReal(NA_REAL), ScalarLogical(true))))[0]; protecti++;

  bool bnarm = LOGICAL(narm)[0];

  int ihasnf =                                                  // plain C tri-state boolean as integer
    LOGICAL(hasnf)[0]==NA_LOGICAL ? 0 :                         // hasnf NA, default, no info about NA
    LOGICAL(hasnf)[0]==TRUE ? 1 :                               // hasnf TRUE, might be some NAs
    -1;                                                         // hasnf FALSE, there should be no NAs // or there must be no NAs for rollmax #5441

  unsigned int ialgo;                                           // decode algo to integer
  if (!strcmp(CHAR(STRING_ELT(algo, 0)), "fast"))
    ialgo = 0;                                                  // fast = 0
  else if (!strcmp(CHAR(STRING_ELT(algo, 0)), "exact"))
    ialgo = 1;                                                  // exact = 1
  else
    error(_("Internal error: invalid %s argument in %s function should have been caught earlier. Please report to the data.table issue tracker."), "algo", "rolling"); // # nocov

  if (verbose) {
    if (ialgo==0)
      Rprintf(_("%s: %d column(s) and %d window(s), if product > 1 then entering parallel execution\n"), __func__, nx, nk);
    else if (ialgo==1)
      Rprintf(_("%s: %d column(s) and %d window(s), not entering parallel execution here because algo='exact' will compute results in parallel\n"), __func__, nx, nk);
  }
  #pragma omp parallel for if (ialgo==0) schedule(dynamic) collapse(2) num_threads(getDTthreads(nx*nk, false))
  for (R_len_t i=0; i<nx; i++) {                                // loop over multiple columns
    for (R_len_t j=0; j<nk; j++) {                              // loop over multiple windows
      if (!badaptive) {
        frollfun(rfun, ialgo, dx[i], inx[i], &dans[i*nk+j], iik[j], ialign, dfill, bnarm, ihasnf, verbose);
      } else {
        frolladaptivefun(rfun, ialgo, dx[i], inx[i], &dans[i*nk+j], ikl[j], dfill, bnarm, ihasnf, verbose);
      }
    }
  }

  ansMsg(dans, nx*nk, verbose, __func__);                       // raise errors and warnings, as of now messages are not being produced

  if (verbose)
    Rprintf(_("%s: processing of %d column(s) and %d window(s) took %.3fs\n"), __func__, nx, nk, omp_get_wtime()-tic);

  UNPROTECT(protecti);
  return isVectorAtomic(obj) && length(ans) == 1 ? VECTOR_ELT(ans, 0) : ans;
}

// helper to find biggest window width for adaptive frollapply
int maxk(int *k, uint64_t len) {
  int mk = k[0];
  for (uint64_t i=1; i<len; i++)
    if (k[i] > mk)
      mk = k[i];
  return mk;
}
SEXP frollapplyR(SEXP fun, SEXP obj, SEXP k, SEXP fill, SEXP align, SEXP adaptive, SEXP rho) {
  int protecti = 0;
  const bool verbose = GetVerbose();

  if (!isFunction(fun))
    error(_("internal error: 'fun' must be a function")); // # nocov
  if (!isEnvironment(rho))
    error(_("internal error: 'rho' should be an environment")); // # nocov

  if (!xlength(obj))
    return(obj);
  double tic = 0;
  if (verbose)
    tic = omp_get_wtime();
  SEXP x = PROTECT(coerceToRealListR(obj)); protecti++;
  R_len_t nx = length(x);

  if (xlength(k) == 0)
    error(_("n must be non 0 length"));

  if (!IS_TRUE_OR_FALSE(adaptive))
    error(_("%s must be TRUE or FALSE"), "adaptive");
  bool badaptive = LOGICAL(adaptive)[0];

  R_len_t nk = 0;
  SEXP ik = R_NilValue;
  SEXP kl = R_NilValue;
  if (!badaptive) {
    if (isNewList(k))
      error(_("n must be integer, list is accepted for adaptive TRUE"));

    if (isInteger(k)) {
      ik = k;
    } else if (isReal(k)) {
      ik = PROTECT(coerceVector(k, INTSXP)); protecti++;
    } else {
      error(_("n must be integer"));
    }

    nk = length(k);
    R_len_t i=0;
    while (i < nk && INTEGER(ik)[i] > 0) i++;
    if (i != nk)
      error(_("n must be positive integer values (> 0)"));
  } else {
    if (isVectorAtomic(k)) {
      kl = PROTECT(allocVector(VECSXP, 1)); protecti++;
      if (isInteger(k)) {
        SET_VECTOR_ELT(kl, 0, k);
      } else if (isReal(k)) {
        SET_VECTOR_ELT(kl, 0, coerceVector(k, INTSXP));
      } else {
        error(_("n must be integer vector or list of integer vectors"));
      }
      nk = 1;
    } else {
      nk = length(k);
      kl = PROTECT(allocVector(VECSXP, nk)); protecti++;
      for (R_len_t i=0; i<nk; i++) {
        if (isInteger(VECTOR_ELT(k, i))) {
          SET_VECTOR_ELT(kl, i, VECTOR_ELT(k, i));
        } else if (isReal(VECTOR_ELT(k, i))) {
          SET_VECTOR_ELT(kl, i, coerceVector(VECTOR_ELT(k, i), INTSXP));
        } else {
          error(_("n must be integer vector or list of integer vectors"));
        }
      }
    }
  }
  int **ikl = (int**)R_alloc(nk, sizeof(int*));
  if (badaptive) {
    for (int j=0; j<nk; j++)
      ikl[j] = INTEGER(VECTOR_ELT(kl, j));
  }

  int* iik = NULL;
  if (!badaptive) {
    if (!isInteger(ik))
      error(_("Internal error: badaptive=%d but ik is not integer"), badaptive); // # nocov
    iik = INTEGER(ik);
  } else {
    // ik is still R_NilValue from initialization. But that's ok as it's only needed below when !badaptive.
  }

  int ialign;
  if (!strcmp(CHAR(STRING_ELT(align, 0)), "right")) {
    ialign = 1;
  } else if (!strcmp(CHAR(STRING_ELT(align, 0)), "center")) {
    ialign = 0;
  } else if (!strcmp(CHAR(STRING_ELT(align, 0)), "left")) {
    ialign = -1;
  } else {
    error(_("Internal error: invalid %s argument in %s function should have been caught earlier. Please report to the data.table issue tracker."), "align", "rolling"); // # nocov
  }

  if (badaptive && ialign==0)
    error(_("using adaptive TRUE and align 'center' is not implemented"));

  if (length(fill) != 1)
    error(_("fill must be a vector of length 1"));
  if (!isInteger(fill) && !isReal(fill) && !isLogical(fill))
    error(_("fill must be numeric or logical"));
  double dfill = REAL(PROTECT(coerceAs(fill, ScalarReal(NA_REAL), ScalarLogical(true))))[0]; protecti++;

  SEXP ans = PROTECT(allocVector(VECSXP, nk * nx)); protecti++;
  if (verbose)
    Rprintf(_("%s: allocating memory for results %dx%d\n"), __func__, nx, nk);
  ans_t *dans = (ans_t *)R_alloc(nx*nk, sizeof(ans_t));
  double** dx = (double**)R_alloc(nx, sizeof(double*));
  uint64_t* inx = (uint64_t*)R_alloc(nx, sizeof(uint64_t));
  for (R_len_t i=0; i<nx; i++) {
    inx[i] = xlength(VECTOR_ELT(x, i));
    for (R_len_t j=0; j<nk; j++) {
      if (badaptive) {
        if (i > 0 && (inx[i]!=inx[i-1]))
          error(_("adaptive rolling function can only process 'x' having equal length of elements, like data.table or data.frame; If you want to call rolling function on list having variable length of elements call it for each field separately"));
        if (xlength(VECTOR_ELT(kl, j))!=inx[0])
          error(_("length of integer vector(s) provided as list to 'n' argument must be equal to number of observations provided in 'x'"));
      }
      SET_VECTOR_ELT(ans, i*nk+j, allocVector(REALSXP, inx[i]));
      dans[i*nk+j] = ((ans_t) { .dbl_v=REAL(VECTOR_ELT(ans, i*nk+j)), .status=0, .message={"\0","\0","\0","\0"} });
    }
    dx[i] = REAL(VECTOR_ELT(x, i));
  }

  double* dw;
  SEXP pw, pc;

  // in the outer loop we handle vectorized k argument
  // for each k we need to allocate a width window object: pw
  // we also need to construct distinct R call pointing to that window
  if (!badaptive) {
    for (R_len_t j=0; j<nk; j++) {
      pw = PROTECT(allocVector(REALSXP, iik[j]));
      dw = REAL(pw);
      pc = PROTECT(LCONS(fun, LCONS(pw, LCONS(R_DotsSymbol, R_NilValue))));
      for (R_len_t i=0; i<nx; i++) {
        frollapply(dx[i], inx[i], dw, iik[j], &dans[i*nk+j], ialign, dfill, pc, rho, verbose);
      }
      UNPROTECT(2);
    }
  } else {
    for (R_len_t j=0; j<nk; j++) {
      pw = PROTECT(allocVector(REALSXP, maxk(ikl[j], inx[0]))); // max window size, inx[0] because inx is constant for adaptive
      SET_GROWABLE_BIT(pw); // so we can set length of window for each observation
      pc = PROTECT(LCONS(fun, LCONS(pw, LCONS(R_DotsSymbol, R_NilValue))));
      for (R_len_t i=0; i<nx; i++) {
        frolladaptiveapply(dx[i], inx[i], pw, ikl[j], &dans[i*nk+j], dfill, pc, rho, verbose);
      }
      UNPROTECT(2);
    }
  }

  if (verbose)
    Rprintf(_("%s: processing of %d column(s) and %d window(s) took %.3fs\n"), __func__, nx, nk, omp_get_wtime()-tic);

  UNPROTECT(protecti);
  return isVectorAtomic(obj) && length(ans) == 1 ? VECTOR_ELT(ans, 0) : ans;
}
