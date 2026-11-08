#ifndef OCT_SPAWN_H
#define OCT_SPAWN_H

/* Starting Octopus's root helper, the way TrollStore apps do: Caz's and Speaka's RootSpawn, in C so the app imports
 * it through module.modulemap (OctopusSpawn) and the host tests can run it. */

#ifdef __cplusplus
extern "C" {
#endif

/* The process started but was killed by a signal, or never started. */
#define OCT_SPAWN_KILLED (-2)
#define OCT_SPAWN_NOT_STARTED (-1)

/* Output kept from the child, at most; anything beyond is read and dropped. */
#define OCT_SPAWN_MAX_OUTPUT 262144

/* Runs `path` with `argv` (argv[0] included, NULL-terminated) and waits for it. With `as_root`, the child is started
 * as root through TrollStore's persona spawn, which needs the com.apple.private.persona-mgmt entitlement. Everything
 * it prints on stdout and stderr is returned in *output, NUL-terminated, to be released with free(); when it can't
 * start, *output says why. Returns its exit code, OCT_SPAWN_NOT_STARTED or OCT_SPAWN_KILLED. */
int oct_spawn(const char *path, const char *const argv[], int as_root, char **output);

#ifdef __cplusplus
}
#endif

#endif
