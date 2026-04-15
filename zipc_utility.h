#pragma once

#include <sys/types.h>
#include <stdio.h>

#include "zipc.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Diff utility ---

enum zipc_diff_kind
{
	ZIPC_DIFF_CONTENT,
	ZIPC_DIFF_ONLY_IN_FIRST,
	ZIPC_DIFF_ONLY_IN_SECOND,
};
typedef enum zipc_diff_kind zipc_diff_kind;

struct zipc_file_diff
{
	const char* name;
	enum zipc_diff_kind kind;
	size_t size_first;
	size_t size_second;
	size_t offset_first_diff; // if ZIPC_DIFF_CONTENT
};
typedef struct zipc_file_diff zipc_file_diff;

struct zipc_comparison
{
	/// Overall status. If this is not ZIPC_SUCCESS, then ignore all other members.
	enum zipc_status status;
	/// Number of entries in the following array.
	int count;
	/// Array of differences. Null if there were no differences.
	const struct zipc_file_diff* differences;
};
typedef struct zipc_comparison zipc_comparison;

/// Compare two zip files and list differences. You must call `zipc_compare_free()` on the
/// return pointer once you are done with the data.
const zipc_comparison* zipc_compare(const char* first, const char* second);

/// Convenience helper function to pretty print the contents of the `zipc_file_diff` array.
void zipc_print_zipc_file_diff(const zipc_comparison* differences, FILE* out);

void zipc_compare_free(const zipc_comparison* differences);

#ifdef __cplusplus
}
#endif
