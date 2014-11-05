/****************************************************************************
 *           Low-level manipulation of the Auto-Extending buffers           *
 *           ----------------------------------------------------           *
 ****************************************************************************/
#include "S4Vectors.h"
#include <stdlib.h>  /* for malloc, free, realloc */

#define MAX_BUFLENGTH_INC (32 * 1024 * 1024)
#define MAX_BUFLENGTH (32 * MAX_BUFLENGTH_INC)


static int debug = 0;

SEXP debug_AEbufs()
{
#ifdef DEBUG_S4VECTORS
	debug = !debug;
	Rprintf("Debug mode turned %s in file %s\n",
		debug ? "on" : "off", __FILE__);
#else
	Rprintf("Debug mode not available in file %s\n", __FILE__);
#endif
	return R_NilValue;
}


/****************************************************************************
 * Core helper functions used for allocation/reallocation of the AEbufs.
 */

static int use_malloc = 0;

SEXP AEbufs_use_malloc(SEXP x)
{
	use_malloc = LOGICAL(x)[0];
	return R_NilValue;
}

/* Guaranteed to return a new buflength > 'buflength', or to raise an error. */
int _get_new_buflength(int buflength)
{
	if (buflength >= MAX_BUFLENGTH)
		error("_get_new_buflength(): MAX_BUFLENGTH reached");
	if (buflength == 0)
		return 128;
	if (buflength <= MAX_BUFLENGTH_INC)
		return 2 * buflength;
	buflength += MAX_BUFLENGTH_INC;
	if (buflength <= MAX_BUFLENGTH)
		return buflength;
	return MAX_BUFLENGTH;
}

static void *malloc_AEbuf(int buflength, size_t size)
{
	void *elts;

	if (buflength == 0)
		return NULL;
	elts = malloc((size_t) buflength * size);
	if (elts == NULL)
		error("S4Vectors internal error in malloc_AEbuf(): "
		      "cannot allocate memory");
	return elts;
}

static void *alloc_AEbuf(int buflength, size_t size)
{
	if (use_malloc)
		return malloc_AEbuf(buflength, size);
	if (buflength == 0)
		return NULL;
	return (void *) R_alloc(buflength, size);
}

static void *realloc_AEbuf(void *elts, int new_buflength,
		int buflength, size_t size)
{
	void *new_elts;

	if (use_malloc) {
		new_elts = realloc(elts, (size_t) new_buflength * size);
		if (new_elts == NULL)
			error("S4Vectors internal error in realloc_AEbuf(): "
			      "cannot reallocate memory");
		return new_elts;
	}
	new_elts = (void *) R_alloc(new_buflength, size);
	return memcpy(new_elts, elts, (size_t) buflength * size);
}


/****************************************************************************
 * IntAE buffers
 *
 * We use a "global IntAE malloc stack" to store a copy of each top-level
 * malloc-based IntAE that is created during the execution of a .Call entry
 * point. The copy must be modified at every reallocation or every time the
 * nb of elements in the buffer (_nelt member) is modified.
 * Every .Call() should start with an empty stack.
 * After the .Call() has returned, the stack must be emptied with
 *     .Call("AEbufs_free", PACKAGE="S4Vectors")
 */

#define	INTAE_MALLOC_STACK_NELT_MAX 2048
static IntAE IntAE_malloc_stack[INTAE_MALLOC_STACK_NELT_MAX];
static int IntAE_malloc_stack_nelt = 0;

static void IntAE_alloc(IntAE *int_ae, int buflength)
{
	int_ae->elts = (int *) alloc_AEbuf(buflength, sizeof(int));
	int_ae->buflength = buflength;
	int_ae->_AE_malloc_stack_idx = -1;
	return;
}

static void IntAE_realloc(IntAE *int_ae)
{
	int new_buflength, idx;

	new_buflength = _get_new_buflength(int_ae->buflength);
	int_ae->elts = (int *) realloc_AEbuf(int_ae->elts, new_buflength,
					int_ae->buflength, sizeof(int));
	int_ae->buflength = new_buflength;
	idx = int_ae->_AE_malloc_stack_idx;
	if (idx >= 0)
		IntAE_malloc_stack[idx] = *int_ae;
	return;
}

int _IntAE_get_nelt(const IntAE *int_ae)
{
	return int_ae->_nelt;
}

int _IntAE_set_nelt(IntAE *int_ae, int nelt)
{
	int idx;

	int_ae->_nelt = nelt;
	idx = int_ae->_AE_malloc_stack_idx;
	if (idx >= 0)
		IntAE_malloc_stack[idx] = *int_ae;
	return nelt;
}

#ifdef DEBUG_S4VECTORS
static void IntAE_print(const IntAE *int_ae)
{
	Rprintf("buflength=%d elts=%p _nelt=%d _AE_malloc_stack_idx=%d",
		int_ae->buflength,
		int_ae->elts,
		int_ae->_nelt,
		int_ae->_AE_malloc_stack_idx);
	return;
}
#endif

/* Must be used on a malloc-based IntAE */
static void IntAE_free(const IntAE *int_ae)
{
	if (int_ae->elts != NULL)
		free(int_ae->elts);
	return;
}

static void reset_IntAE_malloc_stack()
{
	int i;
	const IntAE *int_ae;

	for (i = 0, int_ae = IntAE_malloc_stack;
	     i < IntAE_malloc_stack_nelt;
	     i++, int_ae++)
	{
#ifdef DEBUG_S4VECTORS
		if (debug) {
			Rprintf("IntAE_malloc_stack[%d]: ", i);
			IntAE_print(int_ae);
			Rprintf("\n");
		}
#endif
		IntAE_free(int_ae);
	}
	IntAE_malloc_stack_nelt = 0;
	return;
}

void _IntAE_set_val(const IntAE *int_ae, int val)
{
	int nelt, i, *elt;

	nelt = _IntAE_get_nelt(int_ae);
	for (i = 0, elt = int_ae->elts; i < nelt; i++, elt++)
		*elt = val;
	return;
}

IntAE _new_IntAE(int buflength, int nelt, int val)
{
	IntAE int_ae;
	int idx;

	/* Allocation */
	IntAE_alloc(&int_ae, buflength);
	if (use_malloc) {
		if (IntAE_malloc_stack_nelt >= INTAE_MALLOC_STACK_NELT_MAX)
			error("S4Vectors internal error in _new_IntAE(): "
			      "the \"global IntAE malloc stack\" is full");
		idx = IntAE_malloc_stack_nelt++;
		int_ae._AE_malloc_stack_idx = idx;
		IntAE_malloc_stack[idx] = int_ae;
	}
	/* Initialization */
	_IntAE_set_nelt(&int_ae, nelt);
	_IntAE_set_val(&int_ae, val);
	return int_ae;
}

void _IntAE_insert_at(IntAE *int_ae, int at, int val)
{
	int nelt, i;
	int *elt1;
	const int *elt2;

	nelt = _IntAE_get_nelt(int_ae);
	if (nelt >= int_ae->buflength)
		IntAE_realloc(int_ae);
	elt1 = int_ae->elts + nelt;
	elt2 = elt1 - 1;
	for (i = nelt; i > at; i--)
		*(elt1--) = *(elt2--);
	*elt1 = val;
	_IntAE_set_nelt(int_ae, nelt + 1);
	return;
}

void _IntAE_append(IntAE *int_ae, const int *newvals, int nnewval)
{
	int new_nelt, *dest;

	new_nelt = _IntAE_get_nelt(int_ae) + nnewval;
	while (int_ae->buflength < new_nelt)
		IntAE_realloc(int_ae);
	dest = int_ae->elts + _IntAE_get_nelt(int_ae);
	memcpy(dest, newvals, nnewval * sizeof(int));
	_IntAE_set_nelt(int_ae, new_nelt);
	return;
}

void _IntAE_delete_at(IntAE *int_ae, int at)
{
	int *elt1;
	const int *elt2;
	int nelt0, i2;

	elt1 = int_ae->elts + at;
	elt2 = elt1 + 1;
	nelt0 = _IntAE_get_nelt(int_ae);
	for (i2 = at + 1; i2 < nelt0; i2++)
		*(elt1++) = *(elt2++);
	_IntAE_set_nelt(int_ae, nelt0 - 1);
	return;
}

void _IntAE_shift(const IntAE *int_ae, int shift)
{
	int nelt, i, *elt;

	nelt = _IntAE_get_nelt(int_ae);
	for (i = 0, elt = int_ae->elts; i < nelt; i++, elt++)
		*elt += shift;
	return;
}

/*
 * Left and right IntAE objects must have the same length. This is
 * NOT checked!
 */
void _IntAE_sum_and_shift(const IntAE *int_ae1, const IntAE *int_ae2, int shift)
{
	int nelt, i, *elt1, *elt2;

	nelt = _IntAE_get_nelt(int_ae1);
	for (i = 0, elt1 = int_ae1->elts, elt2 = int_ae2->elts;
	     i < nelt;
	     i++, elt1++, elt2++)
		*elt1 += *elt2 + shift;
	return;
}

void _IntAE_append_shifted_vals(IntAE *int_ae, const int *newvals,
		int nnewval, int shift)
{
	int nelt, new_nelt, i, *elt1;
	const int *elt2;

	nelt = _IntAE_get_nelt(int_ae);
	new_nelt = nelt + nnewval;
	while (int_ae->buflength < new_nelt)
		IntAE_realloc(int_ae);
	for (i = 0, elt1 = int_ae->elts + nelt, elt2 = newvals;
	     i < nnewval;
	     i++, elt1++, elt2++)
		*elt1 = *elt2 + shift;
	_IntAE_set_nelt(int_ae, new_nelt);
	return;
}

void _IntAE_qsort(const IntAE *int_ae, int desc)
{
	_sort_int_array(int_ae->elts, _IntAE_get_nelt(int_ae), desc);
	return;
}

void _IntAE_delete_adjdups(IntAE *int_ae)
{
	int nelt, *elt1;
	const int *elt2;
	int i2;

	nelt = _IntAE_get_nelt(int_ae);
	if (nelt <= 1)
		return;
	elt1 = int_ae->elts;
	elt2 = elt1 + 1;
	for (i2 = 1; i2 < nelt; i2++) {
		if (*elt2 != *elt1) {
			elt1++;
			*elt1 = *elt2;
		}
		elt2++;
	}
	_IntAE_set_nelt(int_ae, elt1 - int_ae->elts + 1);
	return;
}

SEXP _new_INTEGER_from_IntAE(const IntAE *int_ae)
{
	int nelt;
	SEXP ans;

	nelt = _IntAE_get_nelt(int_ae);
	PROTECT(ans = NEW_INTEGER(nelt));
	memcpy(INTEGER(ans), int_ae->elts, sizeof(int) * nelt);
	UNPROTECT(1);
	return ans;
}

static void copy_INTEGER_to_IntAE(SEXP x, IntAE *int_ae)
{
	_IntAE_set_nelt(int_ae, LENGTH(x));
	memcpy(int_ae->elts, INTEGER(x), sizeof(int) * LENGTH(x));
	return;
}

IntAE _new_IntAE_from_INTEGER(SEXP x)
{
	IntAE int_ae;

	int_ae = _new_IntAE(LENGTH(x), 0, 0);
	copy_INTEGER_to_IntAE(x, &int_ae);
	return int_ae;
}

IntAE _new_IntAE_from_CHARACTER(SEXP x, int keyshift)
{
	IntAE int_ae;
	int i, *elt;

#ifdef DEBUG_S4VECTORS
	if (debug) {
		Rprintf("[DEBUG] _new_IntAE_from_CHARACTER(): BEGIN ... "
			"LENGTH(x)=%d keyshift=%d\n",
			LENGTH(x), keyshift);
	}
#endif
	int_ae = _new_IntAE(LENGTH(x), 0, 0);
	_IntAE_set_nelt(&int_ae, int_ae.buflength);
	for (i = 0, elt = int_ae.elts; i < int_ae.buflength; i++, elt++) {
		sscanf(CHAR(STRING_ELT(x, i)), "%d", elt);
		*elt += keyshift;
#ifdef DEBUG_S4VECTORS
		if (debug) {
			if (i < 100
			 || i >= int_ae.buflength - 100)
				Rprintf("[DEBUG] _new_IntAE_from_CHARACTER(): "
					"i=%d key=%s *elt=%d\n",
					i,
					CHAR(STRING_ELT(x, i)), *elt);
		}
#endif
	}
#ifdef DEBUG_S4VECTORS
	if (debug) {
		Rprintf("[DEBUG] _new_IntAE_from_CHARACTER(): END\n");
	}
#endif
	return int_ae;
}


/****************************************************************************
 * IntAEAE buffers
 *
 * We use a "global IntAEAE malloc stack" to store a copy of each top-level
 * malloc-based IntAEAE that is created during the execution of a .Call entry
 * point. The copy must be modified at every reallocation or every time the
 * nb of elements in the buffer (nelt member) is modified.
 * Every .Call() should start with an empty stack.
 * After the .Call() has returned, the stack must be emptied with
 *     .Call("AEbufs_free", PACKAGE="S4Vectors")
 */

#define	INTAEAE_MALLOC_STACK_NELT_MAX 2048
static IntAEAE IntAEAE_malloc_stack[INTAEAE_MALLOC_STACK_NELT_MAX];
static int IntAEAE_malloc_stack_nelt = 0;

static void IntAEAE_alloc(IntAEAE *int_aeae, int buflength)
{
	int_aeae->elts = (IntAE *) alloc_AEbuf(buflength, sizeof(IntAE));
	int_aeae->buflength = buflength;
	int_aeae->_AE_malloc_stack_idx = -1;
	return;
}

static void IntAEAE_realloc(IntAEAE *int_aeae)
{
	int new_buflength, idx;

	new_buflength = _get_new_buflength(int_aeae->buflength);
	int_aeae->elts = (IntAE *) realloc_AEbuf(int_aeae->elts, new_buflength,
					int_aeae->buflength, sizeof(IntAE));
	int_aeae->buflength = new_buflength;
	idx = int_aeae->_AE_malloc_stack_idx;
	if (idx >= 0)
		IntAEAE_malloc_stack[idx] = *int_aeae;
	return;
}

int _IntAEAE_get_nelt(const IntAEAE *int_aeae)
{
	return int_aeae->_nelt;
}

int _IntAEAE_set_nelt(IntAEAE *int_aeae, int nelt)
{
	int idx;

	int_aeae->_nelt = nelt;
	idx = int_aeae->_AE_malloc_stack_idx;
	if (idx >= 0)
		IntAEAE_malloc_stack[idx] = *int_aeae;
	return nelt;
}

/* Must be used on a malloc-based IntAEAE */
static void IntAEAE_free(const IntAEAE *int_aeae)
{
	int nelt, i;
	IntAE *elt;

	nelt = _IntAEAE_get_nelt(int_aeae);
	for (i = 0, elt = int_aeae->elts; i < nelt; i++, elt++)
		IntAE_free(elt);
	if (int_aeae->elts != NULL)
		free(int_aeae->elts);
	return;
}

static void reset_IntAEAE_malloc_stack()
{
	int i;
	const IntAEAE *int_aeae;

	for (i = 0, int_aeae = IntAEAE_malloc_stack;
	     i < IntAEAE_malloc_stack_nelt;
	     i++, int_aeae++)
	{
		IntAEAE_free(int_aeae);
	}
	IntAEAE_malloc_stack_nelt = 0;
	return;
}

IntAEAE _new_IntAEAE(int buflength, int nelt)
{
	IntAEAE int_aeae;
	int idx, i;
	IntAE *elt;

	/* Allocation */
	IntAEAE_alloc(&int_aeae, buflength);
	if (use_malloc) {
		if (IntAEAE_malloc_stack_nelt >= INTAEAE_MALLOC_STACK_NELT_MAX)
			error("S4Vectors internal error in _new_IntAEAE(): "
			      "the \"global IntAEAE malloc stack\" is full");
		idx = IntAEAE_malloc_stack_nelt++;
		int_aeae._AE_malloc_stack_idx = idx;
		IntAEAE_malloc_stack[idx] = int_aeae;
	}
	/* Initialization */
	_IntAEAE_set_nelt(&int_aeae, nelt);
	for (i = 0, elt = int_aeae.elts; i < nelt; i++, elt++) {
		IntAE_alloc(elt, 0);
		_IntAE_set_nelt(elt, 0);
	}
	return int_aeae;
}

void _IntAEAE_insert_at(IntAEAE *int_aeae, int at, const IntAE *int_ae)
{
	int nelt, i;
	IntAE *elt1;
	const IntAE *elt2;

	if (int_ae->_AE_malloc_stack_idx >= 0)
		error("S4Vectors internal error in _IntAEAE_insert_at(): "
		      "cannot insert an IntAE that is in the "
		      "\"global IntAE malloc stack\"");
	nelt = _IntAEAE_get_nelt(int_aeae);
	if (nelt >= int_aeae->buflength)
		IntAEAE_realloc(int_aeae);
	elt1 = int_aeae->elts + nelt;
	elt2 = elt1 - 1;
	for (i = nelt; i > at; i--)
		*(elt1--) = *(elt2--);
	*elt1 = *int_ae;
	_IntAEAE_set_nelt(int_aeae, nelt + 1);
	return;
}

/*
 * Left and right IntAEAE objects must have the same length. This is
 * NOT checked!
 */
void _IntAEAE_eltwise_append(const IntAEAE *int_aeae1, const IntAEAE *int_aeae2)
{
	int nelt, i;
	IntAE *elt1;
	const IntAE *elt2;

	nelt = _IntAEAE_get_nelt(int_aeae1);
	for (i = 0, elt1 = int_aeae1->elts, elt2 = int_aeae2->elts;
	     i < nelt;
	     i++, elt1++, elt2++)
		_IntAE_append(elt1, elt2->elts, _IntAE_get_nelt(elt2));
	return;
}

void _IntAEAE_shift(const IntAEAE *int_aeae, int shift)
{
	int nelt, i;
	IntAE *elt;

	nelt = _IntAEAE_get_nelt(int_aeae);
	for (i = 0, elt = int_aeae->elts; i < nelt; i++, elt++)
		_IntAE_shift(elt, shift);
	return;
}

/*
 * Left and right IntAEAE objects must have the same length. This is
 * NOT checked!
 */
void _IntAEAE_sum_and_shift(const IntAEAE *int_aeae1, const IntAEAE *int_aeae2,
		int shift)
{
	int nelt, i;
	IntAE *elt1;
	const IntAE *elt2;

	nelt = _IntAEAE_get_nelt(int_aeae1);
	for (i = 0, elt1 = int_aeae1->elts, elt2 = int_aeae2->elts;
	     i < nelt;
	     i++, elt1++, elt2++)
		_IntAE_sum_and_shift(elt1, elt2, shift);
	return;
}

/*
 * 'mode' controls how empty list elements should be represented:
 *   0 -> integer(0); 1 -> NULL; 2 -> NA
 */
SEXP _new_LIST_from_IntAEAE(const IntAEAE *int_aeae, int mode)
{
	int nelt, i;
	SEXP ans, ans_elt;
	const IntAE *elt;

	nelt = _IntAEAE_get_nelt(int_aeae);
	PROTECT(ans = NEW_LIST(nelt));
	for (i = 0, elt = int_aeae->elts; i < nelt; i++, elt++) {
		if (_IntAE_get_nelt(elt) != 0 || mode == 0) {
			PROTECT(ans_elt = _new_INTEGER_from_IntAE(elt));
		} else if (mode == 1) {
			continue;
		} else {
			// Not sure new LOGICALs are initialized with NAs,
			// need to check! If not, then LOGICAL(ans_elt)[0] must
			// be set to NA but I don't know how to do this :-/
			PROTECT(ans_elt = NEW_LOGICAL(1));
		}
		SET_VECTOR_ELT(ans, i, ans_elt);
		UNPROTECT(1);
	}
	UNPROTECT(1);
	return ans;
}

IntAEAE _new_IntAEAE_from_LIST(SEXP x)
{
	IntAEAE int_aeae;
	int i;
	IntAE *elt;
	SEXP x_elt;

	int_aeae = _new_IntAEAE(LENGTH(x), 0);
	_IntAEAE_set_nelt(&int_aeae, int_aeae.buflength);
	for (i = 0, elt = int_aeae.elts; i < int_aeae.buflength; i++, elt++) {
		x_elt = VECTOR_ELT(x, i);
		if (TYPEOF(x_elt) != INTSXP)
			error("S4Vectors internal error in "
			      "_new_IntAEAE_from_LIST(): "
			      "not all elements in the list "
			      "are integer vectors");
		IntAE_alloc(elt, LENGTH(x_elt));
		copy_INTEGER_to_IntAE(x_elt, elt);
	}
	return int_aeae;
}

SEXP _IntAEAE_toEnvir(const IntAEAE *int_aeae, SEXP envir, int keyshift)
{
	int nelt, i;
	const IntAE *elt;
	char key[11];
	SEXP value;

	nelt = _IntAEAE_get_nelt(int_aeae);
#ifdef DEBUG_S4VECTORS
	int nkey = 0, cum_length = 0;
	if (debug) {
		Rprintf("[DEBUG] _IntAEAE_toEnvir(): BEGIN ... "
			"int_aeae->_nelt=%d keyshift=%d\n",
			nelt, keyshift);
	}
#endif
	for (i = 0, elt = int_aeae->elts; i < nelt; i++, elt++) {
#ifdef DEBUG_S4VECTORS
		if (debug) {
			if (i < 100 || i >= nelt - 100)
				Rprintf("[DEBUG] _IntAEAE_toEnvir(): "
					"nkey=%d int_aeae->elts[%d]._nelt=%d\n",
					nkey, i, _IntAE_get_nelt(elt));
		}
#endif
		if (_IntAE_get_nelt(elt) == 0)
			continue;
		//snprintf(key, sizeof(key), "%d", i + keyshift);
		snprintf(key, sizeof(key), "%010d", i + keyshift);
#ifdef DEBUG_S4VECTORS
		if (debug) {
			if (i < 100 || i >= nelt - 100)
				Rprintf("[DEBUG] _IntAEAE_toEnvir(): "
					"installing key=%s ... ", key);
		}
#endif
		PROTECT(value = _new_INTEGER_from_IntAE(elt));
		defineVar(install(key), value, envir);
		UNPROTECT(1);
#ifdef DEBUG_S4VECTORS
		if (debug) {
			nkey++;
			cum_length += _IntAE_get_nelt(elt);
			if (i < 100 || i >= nelt - 100)
				Rprintf("OK (nkey=%d cum_length=%d)\n",
					nkey, cum_length);
		}
#endif
	}
#ifdef DEBUG_S4VECTORS
	if (debug) {
		Rprintf("[DEBUG] _IntAEAE_toEnvir(): END "
			"(nkey=%d cum_length=%d)\n", nkey, cum_length);
	}
#endif
	return envir;
}


/****************************************************************************
 * IntPairAE buffers
 *
 * We use a "global IntPairAE malloc stack" to store a copy of each top-level
 * malloc-based IntPairAE that is created during the execution of a .Call
 * entry point. The copy must be modified every time the a or b members are
 * modified.
 * Every .Call() should start with an empty stack.
 * After the .Call() has returned, the stack must be emptied with
 *     .Call("AEbufs_free", PACKAGE="S4Vectors")
 */

#define	INTPAIRAE_MALLOC_STACK_NELT_MAX 2048
static IntPairAE IntPairAE_malloc_stack[INTPAIRAE_MALLOC_STACK_NELT_MAX];
static int IntPairAE_malloc_stack_nelt = 0;

static void IntPairAE_alloc(IntPairAE *intpair_ae, int buflength)
{
	IntAE_alloc(&(intpair_ae->a), buflength);
	IntAE_alloc(&(intpair_ae->b), buflength);
	intpair_ae->_AE_malloc_stack_idx = -1;
	return;
}

int _IntPairAE_get_nelt(const IntPairAE *intpair_ae)
{
	return _IntAE_get_nelt(&(intpair_ae->a));
}

int _IntPairAE_set_nelt(IntPairAE *intpair_ae, int nelt)
{
	int idx;

	_IntAE_set_nelt(&(intpair_ae->a), nelt);
	_IntAE_set_nelt(&(intpair_ae->b), nelt);
	idx = intpair_ae->_AE_malloc_stack_idx;
	if (idx >= 0)
		IntPairAE_malloc_stack[idx] = *intpair_ae;
	return nelt;
}

#ifdef DEBUG_S4VECTORS
static void IntPairAE_print(const IntPairAE *intpair_ae)
{
	IntAE_print(&(intpair_ae->a));
	Rprintf(" ");
	IntAE_print(&(intpair_ae->b));
	Rprintf(" _AE_malloc_stack_idx=%d", intpair_ae->_AE_malloc_stack_idx);
	return;
}
#endif

/* Must be used on a malloc-based IntPairAE */
static void IntPairAE_free(const IntPairAE *intpair_ae)
{
	IntAE_free(&(intpair_ae->a));
	IntAE_free(&(intpair_ae->b));
	return;
}

static void reset_IntPairAE_malloc_stack()
{
	int i;
	const IntPairAE *intpair_ae;

	for (i = 0, intpair_ae = IntPairAE_malloc_stack;
	     i < IntPairAE_malloc_stack_nelt;
	     i++, intpair_ae++)
	{
#ifdef DEBUG_S4VECTORS
		if (debug) {
			Rprintf("IntPairAE_malloc_stack[%d]: ", i);
			IntPairAE_print(intpair_ae);
			Rprintf("\n");
		}
#endif
		IntPairAE_free(intpair_ae);
	}
	IntPairAE_malloc_stack_nelt = 0;
	return;
}

IntPairAE _new_IntPairAE(int buflength, int nelt)
{
	IntPairAE intpair_ae;
	int idx;

	/* Allocation */
	IntPairAE_alloc(&intpair_ae, buflength);
	if (use_malloc) {
		if (IntPairAE_malloc_stack_nelt >=
		    INTPAIRAE_MALLOC_STACK_NELT_MAX)
			error("S4Vectors internal error in _new_IntPairAE(): "
			      "the \"global IntPairAE malloc stack\" is full");
		idx = IntPairAE_malloc_stack_nelt++;
		intpair_ae._AE_malloc_stack_idx = idx;
		IntPairAE_malloc_stack[idx] = intpair_ae;
	}
	/* Elements are NOT initialized */
	_IntPairAE_set_nelt(&intpair_ae, nelt);
	return intpair_ae;
}

void _IntPairAE_insert_at(IntPairAE *intpair_ae, int at, int a, int b)
{
	int idx;

	_IntAE_insert_at(&(intpair_ae->a), at, a);
	_IntAE_insert_at(&(intpair_ae->b), at, b);
	idx = intpair_ae->_AE_malloc_stack_idx;
	if (idx >= 0)
		IntPairAE_malloc_stack[idx] = *intpair_ae;
	return;
}


/****************************************************************************
 * IntPairAEAE buffers
 *
 * We use a "global IntPairAEAE malloc stack" to store a copy of each
 * top-level malloc-based IntPairAEAE that is created during the execution of
 * a .Call entry point. The copy must be modified at every reallocation or
 * every time the nb of elements in the buffer (nelt member) is modified.
 * Every .Call() should start with an empty stack.
 * After the .Call() has returned, the stack must be emptied with
 *     .Call("AEbufs_free", PACKAGE="S4Vectors")
 */

#define	INTPAIRAEAE_MALLOC_STACK_NELT_MAX 2048
static IntPairAEAE IntPairAEAE_malloc_stack[INTPAIRAEAE_MALLOC_STACK_NELT_MAX];
static int IntPairAEAE_malloc_stack_nelt = 0;

static void IntPairAEAE_alloc(IntPairAEAE *intpair_aeae, int buflength)
{
	intpair_aeae->elts = (IntPairAE *) alloc_AEbuf(buflength,
							sizeof(IntPairAE));
	intpair_aeae->buflength = buflength;
	intpair_aeae->_AE_malloc_stack_idx = -1;
	return;
}

static void IntPairAEAE_realloc(IntPairAEAE *intpair_aeae)
{
	int new_buflength, idx;

	new_buflength = _get_new_buflength(intpair_aeae->buflength);
	intpair_aeae->elts = (IntPairAE *) realloc_AEbuf(intpair_aeae->elts,
					new_buflength, intpair_aeae->buflength,
					sizeof(IntPairAE));
	intpair_aeae->buflength = new_buflength;
	idx = intpair_aeae->_AE_malloc_stack_idx;
	if (idx >= 0)
		IntPairAEAE_malloc_stack[idx] = *intpair_aeae;
	return;
}

int _IntPairAEAE_get_nelt(const IntPairAEAE *intpair_aeae)
{
	return intpair_aeae->_nelt;
}

int _IntPairAEAE_set_nelt(IntPairAEAE *intpair_aeae, int nelt)
{
	int idx;

	intpair_aeae->_nelt = nelt;
	idx = intpair_aeae->_AE_malloc_stack_idx;
	if (idx >= 0)
		IntPairAEAE_malloc_stack[idx] = *intpair_aeae;
	return nelt;
}

/* Must be used on a malloc-based IntPairAEAE */
static void IntPairAEAE_free(const IntPairAEAE *intpair_aeae)
{
	int nelt, i;
	IntPairAE *elt;

	nelt = _IntPairAEAE_get_nelt(intpair_aeae);
	for (i = 0, elt = intpair_aeae->elts; i < nelt; i++, elt++)
		IntPairAE_free(elt);
	if (intpair_aeae->elts != NULL)
		free(intpair_aeae->elts);
	return;
}

static void reset_IntPairAEAE_malloc_stack()
{
	int i;
	const IntPairAEAE *intpair_aeae;

	for (i = 0, intpair_aeae = IntPairAEAE_malloc_stack;
	     i < IntPairAEAE_malloc_stack_nelt;
	     i++, intpair_aeae++)
	{
		IntPairAEAE_free(intpair_aeae);
	}
	IntPairAEAE_malloc_stack_nelt = 0;
	return;
}

IntPairAEAE _new_IntPairAEAE(int buflength, int nelt)
{
	IntPairAEAE intpair_aeae;
	int idx, i;
	IntPairAE *elt;

	/* Allocation */
	IntPairAEAE_alloc(&intpair_aeae, buflength);
	if (use_malloc) {
		if (IntPairAEAE_malloc_stack_nelt >=
		    INTPAIRAEAE_MALLOC_STACK_NELT_MAX)
			error("S4Vectors internal error in "
			      "_new_IntPairAEAE(): the \"global "
			      "IntPairAEAE malloc stack\" is full");
		idx = IntPairAEAE_malloc_stack_nelt++;
		intpair_aeae._AE_malloc_stack_idx = idx;
		IntPairAEAE_malloc_stack[idx] = intpair_aeae;
	}
	/* Initialization */
	_IntPairAEAE_set_nelt(&intpair_aeae, nelt);
	for (i = 0, elt = intpair_aeae.elts; i < nelt; i++, elt++) {
		IntPairAE_alloc(elt, 0);
		_IntPairAE_set_nelt(elt, 0);
	}
	return intpair_aeae;
}

void _IntPairAEAE_insert_at(IntPairAEAE *intpair_aeae, int at,
		const IntPairAE *intpair_ae)
{
	int nelt, i;
	IntPairAE *elt1;
	const IntPairAE *elt2;

	if (intpair_ae->_AE_malloc_stack_idx >= 0)
		error("S4Vectors internal error in _IntPairAEAE_insert_at(): "
		      "cannot insert a IntPairAE that is in the "
		      "\"global IntPairAE malloc stack\"");
	nelt = _IntPairAEAE_get_nelt(intpair_aeae);
	if (nelt >= intpair_aeae->buflength)
		IntPairAEAE_realloc(intpair_aeae);
	elt1 = intpair_aeae->elts + nelt;
	elt2 = elt1 - 1;
	for (i = nelt; i > at; i--)
		*(elt1--) = *(elt2--);
	*elt1 = *intpair_ae;
	_IntPairAEAE_set_nelt(intpair_aeae, nelt + 1);
	return;
}


/****************************************************************************
 * CharAE buffers
 *
 * We use a "global CharAE malloc stack" to store a copy of each top-level
 * malloc-based CharAE that is created during the execution of a .Call entry
 * point. The copy must be modified at every reallocation or every time the
 * nb of elements in the buffer (nelt member) is modified.
 * Every .Call() should start with an empty stack.
 * After the .Call() has returned, the stack must be emptied with
 *     .Call("AEbufs_free", PACKAGE="S4Vectors")
 */

#define	CHARAE_MALLOC_STACK_NELT_MAX 2048
static CharAE CharAE_malloc_stack[CHARAE_MALLOC_STACK_NELT_MAX];
static int CharAE_malloc_stack_nelt = 0;

static void CharAE_alloc(CharAE *char_ae, int buflength)
{
	char_ae->elts = (char *) alloc_AEbuf(buflength, sizeof(char));
	char_ae->buflength = buflength;
	char_ae->_AE_malloc_stack_idx = -1;
	return;
}

static void CharAE_realloc(CharAE *char_ae)
{
	int new_buflength, idx;

	new_buflength = _get_new_buflength(char_ae->buflength);
	char_ae->elts = (char *) realloc_AEbuf(char_ae->elts, new_buflength,
					char_ae->buflength, sizeof(char));
	char_ae->buflength = new_buflength;
	idx = char_ae->_AE_malloc_stack_idx;
	if (idx >= 0)
		CharAE_malloc_stack[idx] = *char_ae;
	return;
}

int _CharAE_get_nelt(const CharAE *char_ae)
{
	return char_ae->_nelt;
}

int _CharAE_set_nelt(CharAE *char_ae, int nelt)
{
	int idx;

	char_ae->_nelt = nelt;
	idx = char_ae->_AE_malloc_stack_idx;
	if (idx >= 0)
		CharAE_malloc_stack[idx] = *char_ae;
	return nelt;
}

/* Must be used on a malloc-based CharAE */
static void CharAE_free(const CharAE *char_ae)
{
	if (char_ae->elts != NULL)
		free(char_ae->elts);
	return;
}

static void reset_CharAE_malloc_stack()
{
	int i;
	const CharAE *char_ae;

	for (i = 0, char_ae = CharAE_malloc_stack;
	     i < CharAE_malloc_stack_nelt;
	     i++, char_ae++)
	{
		CharAE_free(char_ae);
	}
	CharAE_malloc_stack_nelt = 0;
	return;
}

CharAE _new_CharAE(int buflength)
{
	CharAE char_ae;
	int idx;

	/* Allocation */
	CharAE_alloc(&char_ae, buflength);
	if (use_malloc) {
		if (CharAE_malloc_stack_nelt >= CHARAE_MALLOC_STACK_NELT_MAX)
			error("S4Vectors internal error in _new_IntAE(): "
			      "the \"global CharAE malloc stack\" is full");
		idx = CharAE_malloc_stack_nelt++;
		char_ae._AE_malloc_stack_idx = idx;
		CharAE_malloc_stack[idx] = char_ae;
	}
	/* Initialization */
	_CharAE_set_nelt(&char_ae, 0);
	return char_ae;
}

CharAE _new_CharAE_from_string(const char *string)
{
	CharAE char_ae;

	char_ae = _new_CharAE(strlen(string));
	_CharAE_set_nelt(&char_ae, char_ae.buflength);
	memcpy(char_ae.elts, string, char_ae.buflength);
	return char_ae;
}

void _CharAE_insert_at(CharAE *char_ae, int at, char c)
{
	int nelt, i;
	char *elt1;
	const char *elt2;

	nelt = _CharAE_get_nelt(char_ae);
	if (nelt >= char_ae->buflength)
		CharAE_realloc(char_ae);
	elt1 = char_ae->elts + nelt;
	elt2 = elt1 - 1;
	for (i = nelt; i > at; i--)
		*(elt1--) = *(elt2--);
	*elt1 = c;
	_CharAE_set_nelt(char_ae, nelt + 1);
	return;
}

void _append_string_to_CharAE(CharAE *char_ae, const char *string)
{
	int nnewval, nelt, new_nelt;
	char *dest;

	nnewval = strlen(string);
	nelt = _CharAE_get_nelt(char_ae);
	new_nelt = nelt + nnewval;
	while (char_ae->buflength < new_nelt)
		CharAE_realloc(char_ae);
	dest = char_ae->elts + nelt;
	memcpy(dest, string, nnewval * sizeof(char));
	_CharAE_set_nelt(char_ae, new_nelt);
	return;
}

/*
 * Delete 'nelt' elements, starting at position 'at'.
 * Doing _CharAE_delete_at(x, at, nelt) is equivalent to doing
 * _CharAE_delete_at(x, at, 1) 'nelt' times.
 */
void _CharAE_delete_at(CharAE *char_ae, int at, int nelt)
{
	char *elt1;
	const char *elt2;
	int nelt0, i2;

	if (nelt == 0)
		return;
	elt1 = char_ae->elts + at;
	elt2 = elt1 + nelt;
	nelt0 = _CharAE_get_nelt(char_ae);
	for (i2 = at + nelt; i2 < nelt0; i2++)
		*(elt1++) = *(elt2++);
	_CharAE_set_nelt(char_ae, nelt0 - nelt);
	return;
}

SEXP _new_RAW_from_CharAE(const CharAE *char_ae)
{
	int nelt;
	SEXP ans;

	if (sizeof(Rbyte) != sizeof(char)) // should never happen!
		error("_new_RAW_from_CharAE(): sizeof(Rbyte) != sizeof(char)");
	nelt = _CharAE_get_nelt(char_ae);
	PROTECT(ans = NEW_RAW(nelt));
	memcpy(RAW(ans), char_ae->elts, sizeof(char) * nelt);
	UNPROTECT(1);
	return ans;
}

/* only until we have a bitset or something smaller than char */
SEXP _new_LOGICAL_from_CharAE(const CharAE *char_ae)
{
	int nelt, i, *ans_elt;
	SEXP ans;
	const char *elt;

	nelt = _CharAE_get_nelt(char_ae);
	PROTECT(ans = NEW_LOGICAL(nelt));
	for (i = 0, ans_elt = LOGICAL(ans), elt = char_ae->elts;
	     i < nelt;
	     i++, ans_elt++, elt++)
	{
		*ans_elt = *elt;
	}
	UNPROTECT(1);
	return ans;
}


/****************************************************************************
 * CharAEAE buffers
 *
 * We use a "global CharAEAE malloc stack" to store a copy of each top-level
 * malloc-based CharAEAE that is created during the execution of a .Call entry
 * point. The copy must be modified at every reallocation or every time the
 * nb of elements in the buffer (nelt member) is modified.
 * Every .Call() should start with an empty stack.
 * After the .Call() has returned, the stack must be emptied with
 *     .Call("AEbufs_free", PACKAGE="S4Vectors")
 */

#define	CHARAEAE_MALLOC_STACK_NELT_MAX 2048
static CharAEAE CharAEAE_malloc_stack[CHARAEAE_MALLOC_STACK_NELT_MAX];
static int CharAEAE_malloc_stack_nelt = 0;

static void CharAEAE_alloc(CharAEAE *char_aeae, int buflength)
{
	char_aeae->elts = (CharAE *) alloc_AEbuf(buflength, sizeof(CharAE));
	char_aeae->buflength = buflength;
	char_aeae->_AE_malloc_stack_idx = -1;
	return;
}

static void CharAEAE_realloc(CharAEAE *char_aeae)
{
	int new_buflength, idx;

	new_buflength = _get_new_buflength(char_aeae->buflength);
	char_aeae->elts = (CharAE *) realloc_AEbuf(char_aeae->elts,
					new_buflength,
					char_aeae->buflength, sizeof(CharAE));
	char_aeae->buflength = new_buflength;
	idx = char_aeae->_AE_malloc_stack_idx;
	if (idx >= 0)
		CharAEAE_malloc_stack[idx] = *char_aeae;
	return;
}

int _CharAEAE_get_nelt(const CharAEAE *char_aeae)
{
	return char_aeae->_nelt;
}

int _CharAEAE_set_nelt(CharAEAE *char_aeae, int nelt)
{
	int idx;

	char_aeae->_nelt = nelt;
	idx = char_aeae->_AE_malloc_stack_idx;
	if (idx >= 0)
		CharAEAE_malloc_stack[idx] = *char_aeae;
	return nelt;
}

/* Must be used on a malloc-based CharAEAE */
static void CharAEAE_free(const CharAEAE *char_aeae)
{
	int nelt, i;
	CharAE *elt;

	nelt = _CharAEAE_get_nelt(char_aeae);
	for (i = 0, elt = char_aeae->elts; i < nelt; i++, elt++)
		CharAE_free(elt);
	if (char_aeae->elts != NULL)
		free(char_aeae->elts);
	return;
}

static void reset_CharAEAE_malloc_stack()
{
	int i;
	const CharAEAE *char_aeae;

	for (i = 0, char_aeae = CharAEAE_malloc_stack;
	     i < CharAEAE_malloc_stack_nelt;
	     i++, char_aeae++)
	{
		CharAEAE_free(char_aeae);
	}
	CharAEAE_malloc_stack_nelt = 0;
	return;
}

CharAEAE _new_CharAEAE(int buflength, int nelt)
{
	CharAEAE char_aeae;
	int idx, i;
	CharAE *elt;

	/* Allocation */
	CharAEAE_alloc(&char_aeae, buflength);
	if (use_malloc) {
		if (CharAEAE_malloc_stack_nelt >=
		    CHARAEAE_MALLOC_STACK_NELT_MAX)
			error("S4Vectors internal error in _new_CharAEAE(): "
			      "the \"global CharAEAE malloc stack\" is full");
		idx = CharAEAE_malloc_stack_nelt++;
		char_aeae._AE_malloc_stack_idx = idx;
		CharAEAE_malloc_stack[idx] = char_aeae;
	}
	/* Initialization */
	_CharAEAE_set_nelt(&char_aeae, nelt);
	for (i = 0, elt = char_aeae.elts; i < nelt; i++, elt++) {
		CharAE_alloc(elt, 0);
		_CharAE_set_nelt(elt, 0);
	}
	return char_aeae;
}

void _CharAEAE_insert_at(CharAEAE *char_aeae, int at, const CharAE *char_ae)
{
	int nelt, i;
	CharAE *elt1;
	const CharAE *elt2;

	if (char_ae->_AE_malloc_stack_idx >= 0)
		error("S4Vectors internal error in _CharAEAE_insert_at(): "
		      "cannot insert a CharAE that is in the "
		      "\"global CharAE malloc stack\"");
	nelt = _CharAEAE_get_nelt(char_aeae);
	if (nelt >= char_aeae->buflength)
		CharAEAE_realloc(char_aeae);
	elt1 = char_aeae->elts + nelt;
	elt2 = elt1 - 1;
	for (i = nelt; i > at; i--)
		*(elt1--) = *(elt2--);
	*elt1 = *char_ae;
	_CharAEAE_set_nelt(char_aeae, nelt + 1);
	return;
}

void _append_string_to_CharAEAE(CharAEAE *char_aeae, const char *string)
{
	CharAE char_ae;

	CharAE_alloc(&char_ae, strlen(string));
	_CharAE_set_nelt(&char_ae, char_ae.buflength);
	memcpy(char_ae.elts, string, char_ae.buflength);
	_CharAEAE_insert_at(char_aeae, _CharAEAE_get_nelt(char_aeae), &char_ae);
	return;
}

SEXP _new_CHARACTER_from_CharAEAE(const CharAEAE *char_aeae)
{
	int nelt, i;
	SEXP ans, ans_elt;
	CharAE *elt;

	nelt = _CharAEAE_get_nelt(char_aeae);
	PROTECT(ans = NEW_CHARACTER(nelt));
	for (i = 0, elt = char_aeae->elts; i < nelt; i++, elt++) {
		PROTECT(ans_elt = mkCharLen(elt->elts, _CharAE_get_nelt(elt)));
		SET_STRING_ELT(ans, i, ans_elt);
		UNPROTECT(1);
	}
	UNPROTECT(1);
	return ans;
}


/****************************************************************************
 * Freeing the malloc-based AEbufs.
 */

SEXP AEbufs_free()
{
	reset_IntAE_malloc_stack();
	reset_IntAEAE_malloc_stack();
	reset_IntPairAE_malloc_stack();
	reset_IntPairAEAE_malloc_stack();
	reset_CharAE_malloc_stack();
	reset_CharAEAE_malloc_stack();
	return R_NilValue;
}

