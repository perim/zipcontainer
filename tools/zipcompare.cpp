#include "zipc.h"
#include "zipc_utility.h"

#include <stdio.h>

int main(int argc, char** argv)
{
	if (argc != 3)
	{
		fprintf(stderr, "usage: %s <first.zip> <second.zip>\n", argc > 0 ? argv[0] : "zipcompare");
		return 2;
	}

	const zipc_comparison* comparison = zipc_compare(argv[1], argv[2]);
	if (!comparison)
	{
		fprintf(stderr, "comparison failed: out of memory\n");
		return 2;
	}

	if (comparison->status != ZIPC_SUCCESS)
	{
		fprintf(stderr, "comparison failed: %s\n", zipc_strerror(comparison->status));
		zipc_compare_free(comparison);
		return 2;
	}

	zipc_print_zipc_file_diff(comparison, stdout);
	const int exit_code = comparison->count == 0 ? 0 : 1;
	zipc_compare_free(comparison);
	return exit_code;
}
