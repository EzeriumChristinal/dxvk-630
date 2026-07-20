#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include "memory-stream.h"

bool
memory_stream_open(struct memory_stream *m)
{
	*m = (struct memory_stream){ 0 };
	m->fp = tmpfile();
	return m->fp != NULL;
}

char *
memory_stream_close(struct memory_stream *m)
{
	char *str = NULL;
	long len;
	if (!m->fp)
		goto out;
	len = ftell(m->fp);
	if (len < 0)
		goto out;
	str = malloc((size_t)len + 1);
	if (!str)
		goto out;
	rewind(m->fp);
	fread(str, 1, (size_t)len, m->fp);
	str[len] = '\0';
out:
	if (m->fp)
		fclose(m->fp);
	*m = (struct memory_stream){ 0 };
	return str;
}

void
memory_stream_cleanup(struct memory_stream *m)
{
	free(memory_stream_close(m));
}
