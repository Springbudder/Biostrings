#include "Biostrings.h"
#include <stdlib.h> /* for abs() */

static ByteTrTable byte2offset;

static SEXP init_numeric_vector(int n, double val, int as_integer)
{
	SEXP ans;
	int i;

	if (as_integer) {
		PROTECT(ans = NEW_INTEGER(n));
		for (i = 0; i < n; i++)
			INTEGER(ans)[i] = (int) val;
	} else {
		PROTECT(ans = NEW_NUMERIC(n));
		for (i = 0; i < n; i++)
			REAL(ans)[i] = val;
	}
	UNPROTECT(1);
	return ans;
}

static SEXP init_numeric_matrix(int nrow, int ncol, double val, int as_integer)
{
	SEXP ans;
	int i, n;

	n = nrow * ncol;
	if (as_integer) {
		PROTECT(ans = allocMatrix(INTSXP, nrow, ncol));
		for (i = 0; i < n; i++)
			INTEGER(ans)[i] = (int) val;
	} else {
		PROTECT(ans = allocMatrix(REALSXP, nrow, ncol));
		for (i = 0; i < n; i++)
			REAL(ans)[i] = val;
	}
	UNPROTECT(1);
	return ans;
}

static int get_ans_width(SEXP codes, int with_other)
{
	int width, i;

	if (codes == R_NilValue)
		return 256;
	_init_byte2offset_with_INTEGER(byte2offset, codes, 1);
	width = LENGTH(codes);
	if (with_other) {
		for (i = 0; i < BYTETRTABLE_LENGTH; i++)
			if (byte2offset[i] == NA_INTEGER)
				byte2offset[i] = width;
		width++;
	}
	return width;
}

static void update_letter_freqs(int *row, int nrow, const RoSeq *X, SEXP codes)
{
	int i, offset;
	const char *c;

	for (i = 0, c = X->elts; i < X->nelt; i++, c++) {
		offset = (unsigned char) *c;
		if (codes != R_NilValue) {
			offset = byte2offset[offset];
			if (offset == NA_INTEGER)
				continue;
		}
		row[offset * nrow]++;
	}
	return;
}

/* Note that calling update_letter_freqs2() with shift = 0, nrow = 0 and
   ncol = X->nelt is equivalent to calling update_letter_freqs() */
static void update_letter_freqs2(int *mat, const RoSeq *X, SEXP codes,
		int shift, int nrow, int ncol)
{
	int i1, i2, j1, j2, *col, i, offset;
	const char *c;

	if (abs(shift) >= ncol)
		return;
	/* i1, i2 are 0-based indices in X->elts
	   (range i1 <= i < i2 must be safe) */
	i1 = 0;
	i2 = X->nelt;
	/* j1, j2 are 0-based column indices in the freqs matrix
	   (range j1 <= j < j2 must be safe) */
	j1 = i1 + shift;
	j2 = i2 + shift;
	if (j1 < 0) {
		i1 -= j1;
		j1 = 0;
	}
	if (j2 > ncol) {
		i2 -= j2 - ncol;
		/* j2 = ncol; not needed */
	}
	c = X->elts + i1;
	col = mat + j1 * nrow;
	for (i = i1; i < i2; i++, c++, col += nrow) {
		offset = (unsigned char) *c;
		if (codes != R_NilValue) {
			offset = byte2offset[offset];
			if (offset == NA_INTEGER)
				continue;
		}
		col[offset]++;
	}
	return;
}

static void update_oligo_freqs(SEXP mat, int row, int nrow,
		TwobitEncodingBuffer *teb, const RoSeq *X)
{
	int i, offset;
	const char *c;
	int *row_iptr;
	double *row_dptr;

	_reset_twobit_signature(teb);
	switch (TYPEOF(mat)) {
	    case INTSXP:
		row_iptr = INTEGER(mat) + row;
		for (i = 0, c = X->elts; i < X->nelt; i++, c++) {
			offset = _next_twobit_signature(teb, c);
			if (offset == NA_INTEGER)
				continue;
			row_iptr[offset * nrow]++;
		}
		break;
	    case REALSXP:
		row_dptr = REAL(mat) + row;
		for (i = 0, c = X->elts; i < X->nelt; i++, c++) {
			offset = _next_twobit_signature(teb, c);
			if (offset == NA_INTEGER)
				continue;
			row_dptr[offset * nrow] += 1.00;
		}
		break;
	}
	return;
}

static void normalize_oligo_freqs(SEXP mat, int nrow, int ncol)
{
	int i, j;
	double sum;

	for (i = 0; i < nrow; i++) {
		sum = 0.00;
		for (j = 0; j < ncol; j++)
			sum += REAL(mat)[i + j * nrow];
		if (sum == 0.00)
			continue;
		for (j = 0; j < ncol; j++)
			REAL(mat)[i + j * nrow] /= sum;
	}
	return;
}

static SEXP append_other_to_names(SEXP codes)
{
	SEXP names, name, codes_names;
	int i;

	PROTECT(names = NEW_CHARACTER(LENGTH(codes) + 1));
	codes_names = GET_NAMES(codes);
	for (i = 0; i < LENGTH(codes); i++) {
		if (codes_names == R_NilValue)
			PROTECT(name = mkChar(""));
		else
			PROTECT(name = duplicate(STRING_ELT(codes_names, i)));
		SET_STRING_ELT(names, i, name);
		UNPROTECT(1);
	}
	SET_STRING_ELT(names, i, mkChar("other"));
	UNPROTECT(1);
	return names;
}

static void set_names(SEXP x, SEXP codes, int with_other, int collapse, int which_names)
{
	SEXP names, codes_names, dim_names;

	if (codes == R_NilValue)
		return;
	if (with_other) {
		PROTECT(names = append_other_to_names(codes));
	} else {
		codes_names = GET_NAMES(codes);
		PROTECT(names = duplicate(codes_names));
	}
	if (collapse) {
		SET_NAMES(x, names);
	} else {
		PROTECT(dim_names = NEW_LIST(2));
		SET_ELEMENT(dim_names, 1 - which_names, R_NilValue);
		SET_ELEMENT(dim_names, which_names, names);
		SET_DIMNAMES(x, dim_names);
		UNPROTECT(1);
	}
	UNPROTECT(1);
	return;
}

static void oligo_freqs_as_array(SEXP x, int width, SEXP base_labels)
{
	SEXP dim, dim_names;
	int i;

	PROTECT(dim = NEW_INTEGER(width));
	for (i = 0; i < width; i++)
		INTEGER(dim)[i] = 4;
	SET_DIM(x, dim);
	UNPROTECT(1);
	if (base_labels == R_NilValue)
		return;
	PROTECT(dim_names = NEW_LIST(width));
	for (i = 0; i < width; i++)
		SET_ELEMENT(dim_names, i, duplicate(base_labels));
	SET_DIMNAMES(x, dim_names);
	UNPROTECT(1);
	return;
}

static SEXP mk_all_oligos(int width, SEXP base_letters, int invert_twobit_order)
{
	SEXP ans;
	int ans_length, i, j, k, twobit_sign;
	char ans_elt_buf[16], c;

	if (width >= sizeof(ans_elt_buf))
		error("mk_all_oligos(): width >= sizeof(ans_elt_buf))");
	if (LENGTH(base_letters) != 4)
		error("mk_all_oligos(): 'base_letters' must be of length 4");
	ans_length = 1 << (width * 2); /* 4^width */
	PROTECT(ans = NEW_CHARACTER(ans_length));
	ans_elt_buf[width] = 0;
	for (i = 0; i < ans_length; i++) {
		twobit_sign = i;
		for (j = 0; j < width; j++) {
			k = twobit_sign & 3;
			c = CHAR(STRING_ELT(base_letters, k))[0];
			if (invert_twobit_order)
				ans_elt_buf[j] = c;
			else
				ans_elt_buf[width - 1 - j] = c;
			twobit_sign >>= 2;
		}
		SET_STRING_ELT(ans, i, mkChar(ans_elt_buf));
	}
	UNPROTECT(1);
	return ans;
}

static void format_oligo_freqs(SEXP x, int width,
		SEXP base_labels, int invert_twobit_order, int as_array)
{
	SEXP flat_names;

	if (as_array) {
		oligo_freqs_as_array(x, width, base_labels);
		return;
	}
	if (base_labels == R_NilValue)
		return;
	PROTECT(flat_names = mk_all_oligos(width, base_labels, invert_twobit_order));
	SET_NAMES(x, flat_names);
	UNPROTECT(1);
	return;
}

static void set_oligo_freqs_colnames(SEXP x, int width,
		SEXP base_labels, int invert_twobit_order)
{
	SEXP flat_names, dim_names;

	if (base_labels == R_NilValue)
		return;
	PROTECT(flat_names = mk_all_oligos(width, base_labels, invert_twobit_order));
	PROTECT(dim_names = NEW_LIST(2));
	SET_ELEMENT(dim_names, 0, R_NilValue);
	SET_ELEMENT(dim_names, 1, flat_names);
	SET_DIMNAMES(x, dim_names);
	UNPROTECT(2);
	return;
}



/****************************************************************************
 *                        --- .Call ENTRY POINTS ---                        *
 ****************************************************************************/

SEXP XString_letter_frequency(SEXP x, SEXP codes, SEXP with_other)
{
	SEXP ans;
	int ans_width;
	RoSeq X;

	ans_width = get_ans_width(codes, LOGICAL(with_other)[0]);
	PROTECT(ans = NEW_INTEGER(ans_width));
	memset(INTEGER(ans), 0, LENGTH(ans) * sizeof(int));
	X = _get_XString_asRoSeq(x);
	update_letter_freqs(INTEGER(ans), 1, &X, codes);
	set_names(ans, codes, LOGICAL(with_other)[0], 1, 1);
	UNPROTECT(1);
	return ans;
}

SEXP XStringSet_letter_frequency(SEXP x, SEXP codes, SEXP with_other,
		SEXP collapse)
{
	SEXP ans;
	int ans_width, x_length, *ans_row, i;
	CachedXStringSet cached_x;
	RoSeq x_elt;

	ans_width = get_ans_width(codes, LOGICAL(with_other)[0]);
	x_length = _get_XStringSet_length(x);
	cached_x = _new_CachedXStringSet(x);
	if (LOGICAL(collapse)[0]) {
		PROTECT(ans = NEW_INTEGER(ans_width));
		ans_row = INTEGER(ans);
		memset(ans_row, 0, LENGTH(ans) * sizeof(int));
		for (i = 0; i < x_length; i++) {
			x_elt = _get_CachedXStringSet_elt_asRoSeq(&cached_x, i);
			update_letter_freqs(ans_row, 1, &x_elt, codes);
		}
	} else {
		PROTECT(ans = allocMatrix(INTSXP, x_length, ans_width));
		ans_row = INTEGER(ans);
		memset(ans_row, 0, LENGTH(ans) * sizeof(int));
		for (i = 0; i < x_length; i++, ans_row++) {
			x_elt = _get_CachedXStringSet_elt_asRoSeq(&cached_x, i);
			update_letter_freqs(ans_row, x_length, &x_elt, codes);
		}
	}
	set_names(ans, codes, LOGICAL(with_other)[0], LOGICAL(collapse)[0], 1);
	UNPROTECT(1);
	return ans;
}

SEXP XString_oligo_frequency(SEXP x, SEXP base_codes, SEXP width,
		SEXP freq, SEXP fast_moving_side,
		SEXP as_array, SEXP with_labels)
{
	SEXP ans, base_labels;
	TwobitEncodingBuffer teb;
	int width0, as_integer, invert_twobit_order, as_array0, ans_width;
	RoSeq X;

	width0 = INTEGER(width)[0];
	as_integer = !LOGICAL(freq)[0];
	invert_twobit_order = strcmp(CHAR(STRING_ELT(fast_moving_side, 0)), "right") != 0;
	teb = _new_TwobitEncodingBuffer(base_codes, width0, invert_twobit_order);
	as_array0 = LOGICAL(as_array)[0];
	base_labels = LOGICAL(with_labels)[0] ? GET_NAMES(base_codes) : R_NilValue;
	ans_width = 1 << (width0 * 2); /* 4^width0 */
	PROTECT(ans = init_numeric_vector(ans_width, 0.00, as_integer));
	X = _get_XString_asRoSeq(x);
	update_oligo_freqs(ans, 0, 1, &teb, &X);
	if (!as_integer)
		normalize_oligo_freqs(ans, 1, ans_width);
	format_oligo_freqs(ans, width0, base_labels, invert_twobit_order, as_array0);
	UNPROTECT(1);
	return ans;
}

SEXP XStringSet_oligo_frequency(SEXP x, SEXP base_codes, SEXP width,
		SEXP freq, SEXP fast_moving_side,
		SEXP as_array, SEXP with_labels, SEXP simplify_as)
{
	SEXP ans, base_labels, ans_elt;
	TwobitEncodingBuffer teb;
	int width0, as_integer, invert_twobit_order, as_array0, ans_width, x_length, i;
	const char *simplify_as0;
	CachedXStringSet cached_x;
	RoSeq x_elt;

	width0 = INTEGER(width)[0];
	as_integer = !LOGICAL(freq)[0];
	invert_twobit_order = strcmp(CHAR(STRING_ELT(fast_moving_side, 0)), "right") != 0;
	simplify_as0 = CHAR(STRING_ELT(simplify_as, 0));
	teb = _new_TwobitEncodingBuffer(base_codes, width0, invert_twobit_order);
	as_array0 = LOGICAL(as_array)[0];
	base_labels = LOGICAL(with_labels)[0] ? GET_NAMES(base_codes) : R_NilValue;
	ans_width = 1 << (width0 * 2); /* 4^width0 */
	x_length = _get_XStringSet_length(x);
	cached_x = _new_CachedXStringSet(x);
	if (strcmp(simplify_as0, "matrix") == 0) {  /* the default */
		PROTECT(ans = init_numeric_matrix(x_length, ans_width, 0.00, as_integer));
		for (i = 0; i < x_length; i++) {
			x_elt = _get_CachedXStringSet_elt_asRoSeq(&cached_x, i);
			update_oligo_freqs(ans, i, x_length, &teb, &x_elt);
		}
		if (!as_integer)
			normalize_oligo_freqs(ans, x_length, ans_width);
		set_oligo_freqs_colnames(ans, width0, base_labels, invert_twobit_order);
		UNPROTECT(1);
		return ans;
	}
	if (strcmp(simplify_as0, "collapsed") == 0) {
		PROTECT(ans = init_numeric_vector(ans_width, 0.00, as_integer));
		for (i = 0; i < x_length; i++) {
			x_elt = _get_CachedXStringSet_elt_asRoSeq(&cached_x, i);
			update_oligo_freqs(ans, 0, 1, &teb, &x_elt);
		}
		if (!as_integer)
			normalize_oligo_freqs(ans, 1, ans_width);
		format_oligo_freqs(ans, width0, base_labels, invert_twobit_order, as_array0);
		UNPROTECT(1);
		return ans;
	}
	PROTECT(ans = NEW_LIST(x_length));
	for (i = 0; i < x_length; i++) {
		PROTECT(ans_elt = init_numeric_vector(ans_width, 0.00, as_integer));
		x_elt = _get_CachedXStringSet_elt_asRoSeq(&cached_x, i);
		update_oligo_freqs(ans_elt, 0, 1, &teb, &x_elt);
		if (!as_integer)
			normalize_oligo_freqs(ans_elt, 1, ans_width);
		format_oligo_freqs(ans_elt, width0, base_labels, invert_twobit_order, as_array0);
		SET_ELEMENT(ans, i, ans_elt);
		UNPROTECT(1);
	}
	UNPROTECT(1);
	return ans;
}

SEXP XStringSet_letter_frequency_by_pos(SEXP x, SEXP codes, SEXP with_other,
		SEXP shift, SEXP width)
{
	SEXP ans;
	int ans_nrow, ans_ncol, ans_length, x_length, i, k, s, x_elt_end;
	CachedXStringSet cached_x;
	RoSeq x_elt;

	ans_nrow = get_ans_width(codes, LOGICAL(with_other)[0]);
	x_length = _get_XStringSet_length(x);
	cached_x = _new_CachedXStringSet(x);
	if (width == R_NilValue) {
		if (x_length == 0)
			error("'x' has no element and 'width' is NULL");
		if (LENGTH(shift) == 0)
			error("'shift' has no element");
		ans_ncol = 0;
		for (i = k = 0; i < x_length; i++, k++) {
			if (k >= LENGTH(shift))
				k = 0; /* recycle */
			s = INTEGER(shift)[k];
			if (s == NA_INTEGER)
				error("'shift' contains NAs");
			x_elt = _get_CachedXStringSet_elt_asRoSeq(&cached_x, i);
			x_elt_end = x_elt.nelt + s;
			if (x_elt_end > ans_ncol)
				ans_ncol = x_elt_end;
		}
	} else {
		if (x_length != 0 && LENGTH(shift) == 0)
			error("'shift' has no element");
		ans_ncol = INTEGER(width)[0];
	}
	ans_length = ans_nrow * ans_ncol;
	PROTECT(ans = allocMatrix(INTSXP, ans_nrow, ans_ncol));
	memset(INTEGER(ans), 0, ans_length * sizeof(int));
	for (i = k = 0; i < x_length; i++, k++) {
		if (k >= LENGTH(shift))
			k = 0; /* recycle */
		s = INTEGER(shift)[k];
		if (s == NA_INTEGER)
			error("'shift' contains NAs");
		x_elt = _get_CachedXStringSet_elt_asRoSeq(&cached_x, i);
		update_letter_freqs2(INTEGER(ans), &x_elt, codes, s, ans_nrow, ans_ncol);
	}
	set_names(ans, codes, LOGICAL(with_other)[0], 0, 0);
	UNPROTECT(1);
	return ans;
}

