/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef __SOC_QCOM_EDL_ON_DEMAND_H
#define __SOC_QCOM_EDL_ON_DEMAND_H

/*
 * Request the built-in backend's one-shot full dump. A negative result means
 * the request was rejected before acceptance. Zero means a built-in tasklet
 * was scheduled; it does not report successful hardware entry or dump capture.
 */
int qcom_edl_request_full_dump(void);

#endif /* __SOC_QCOM_EDL_ON_DEMAND_H */
