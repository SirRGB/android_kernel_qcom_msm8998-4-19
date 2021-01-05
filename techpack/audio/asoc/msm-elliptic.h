/**
 * Elliptic Labs
 */

#ifndef _MSM_PCM_ELUS
#define _MSM_PCM_ELUS

#include <dsp/apr_elliptic.h>

struct snd_soc_component;

unsigned int elliptic_add_platform_controls(struct snd_soc_component *component);

#endif
