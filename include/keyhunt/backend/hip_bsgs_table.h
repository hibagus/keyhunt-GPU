#pragma once
// Source compatibility for C07-C13 clients. The selected build supplies one
// native backend; new code should use the backend-neutral Gpu API.
#include "keyhunt/backend/gpu_bsgs_table.h"
namespace keyhunt::backend { using HipBsgsTable = GpuBsgsTable; }
