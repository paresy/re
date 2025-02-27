#include <re_types.h>
#include <re_mem.h>
#include <re_thread.h>


static void mutex_destructor(void *data)
{
	mtx_t *mtx = data;

	mtx_destroy(mtx);
}


int mutex_alloc(mtx_t **mtx)
{
	mtx_t *m;
	int err;

	if (!mtx)
		return EINVAL;

	m = mem_alloc(sizeof(mtx_t), NULL);
	if (!m)
		return ENOMEM;

	err = mtx_init(m, mtx_plain);
	if (err != thrd_success) {
		err = ENOMEM;
		goto out;
	}

	mem_destructor(m, mutex_destructor);

	*mtx = m;

out:
	if (err)
		mem_deref(m);

	return err;
}


int thread_create_name(thrd_t *thr, const char *name, thrd_start_t func,
		     void *arg)
{
	(void)name;

	if (!thr || !func)
		return EINVAL;

	return (thrd_create(thr, func, arg) == thrd_success) ? 0 : EAGAIN;
}
