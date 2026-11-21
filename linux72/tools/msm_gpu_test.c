#define _GNU_SOURCE
#include <drm/drm.h>
#include <drm/msm_drm.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

static inline uint32_t pm4_parity(uint32_t val)
{
	return (0x9669u >> (0xFu & (val ^
		(val >> 4) ^ (val >> 8) ^ (val >> 12) ^
		(val >> 16) ^ (val >> 20) ^ (val >> 24) ^
		(val >> 28)))) & 1u;
}

#define CP_TYPE7_PKT 0x70000000u
#define CP_MEM_WRITE 0x3du
#define PKT7(opcode, cnt) \
	(CP_TYPE7_PKT | ((uint32_t)(cnt) << 0) | (pm4_parity(cnt) << 15) | \
	 (((uint32_t)(opcode) & 0x7Fu) << 16) | (pm4_parity(opcode) << 23))

static int get_param(int fd, uint32_t param, uint64_t *val)
{
	struct drm_msm_param p = {
		.pipe = MSM_PIPE_3D0,
		.param = param,
	};
	if (ioctl(fd, DRM_IOCTL_MSM_GET_PARAM, &p) < 0)
		return -errno;
	*val = p.value;
	return 0;
}

static int alloc_bo(int fd, size_t size, uint32_t *handle, uint64_t *iova, void **map)
{
	struct drm_msm_gem_new gn = {
		.size = size,
		.flags = MSM_BO_WC,
	};
	struct drm_msm_gem_info gi = { 0 };
	void *ptr;

	if (ioctl(fd, DRM_IOCTL_MSM_GEM_NEW, &gn) < 0) {
		perror("DRM_IOCTL_MSM_GEM_NEW");
		return -1;
	}
	*handle = gn.handle;

	gi.handle = gn.handle;
	gi.info = MSM_INFO_GET_IOVA;
	if (ioctl(fd, DRM_IOCTL_MSM_GEM_INFO, &gi) < 0) {
		perror("MSM_INFO_GET_IOVA");
		return -1;
	}
	*iova = gi.value;

	memset(&gi, 0, sizeof(gi));
	gi.handle = gn.handle;
	gi.info = MSM_INFO_GET_OFFSET;
	if (ioctl(fd, DRM_IOCTL_MSM_GEM_INFO, &gi) < 0) {
		perror("MSM_INFO_GET_OFFSET");
		return -1;
	}

	ptr = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)gi.value);
	if (ptr == MAP_FAILED) {
		perror("mmap");
		return -1;
	}
	*map = ptr;
	return 0;
}

int main(int argc, char **argv)
{
	const char *dev = argc > 1 ? argv[1] : "/dev/dri/renderD128";
	char name[64] = {0}, date[64] = {0}, desc[128] = {0};
	struct drm_version ver = {
		.name_len = sizeof(name) - 1,
		.name = name,
		.date_len = sizeof(date) - 1,
		.date = date,
		.desc_len = sizeof(desc) - 1,
		.desc = desc,
	};
	uint64_t gpu_id = 0, chip_id = 0, gmem_size = 0, max_freq = 0, ts0 = 0, ts1 = 0, prios = 0;
	uint32_t cmd_h = 0, dst_h = 0;
	uint64_t cmd_iova = 0, dst_iova = 0;
	void *cmd_map = NULL, *dst_map = NULL;
	volatile uint32_t *cmd, *dst;
	struct drm_msm_submitqueue sq = { .flags = 0, .prio = 0 };
	struct drm_msm_gem_submit_bo bos[2];
	struct drm_msm_gem_submit_cmd cmds[1];
	struct drm_msm_gem_submit sub = { 0 };
	struct drm_msm_wait_fence wf = { 0 };
	struct timespec now;
	int fd;

	fd = open(dev, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(dev);
		return 1;
	}

	if (ioctl(fd, DRM_IOCTL_VERSION, &ver) < 0) {
		perror("DRM_IOCTL_VERSION");
		return 1;
	}
	printf("DRM driver: %s %d.%d.%d (%s) %s\n",
	       name, ver.version_major, ver.version_minor, ver.version_patchlevel, date, desc);

	get_param(fd, MSM_PARAM_GPU_ID, &gpu_id);
	get_param(fd, MSM_PARAM_CHIP_ID, &chip_id);
	get_param(fd, MSM_PARAM_GMEM_SIZE, &gmem_size);
	get_param(fd, MSM_PARAM_MAX_FREQ, &max_freq);
	get_param(fd, MSM_PARAM_PRIORITIES, &prios);
	get_param(fd, MSM_PARAM_TIMESTAMP, &ts0);
	printf("Params: gpu_id=%llu chip_id=0x%llx gmem=%llu KiB max_freq=%llu Hz prios=%llu ts0=%llu\n",
	       (unsigned long long)gpu_id, (unsigned long long)chip_id,
	       (unsigned long long)(gmem_size / 1024), (unsigned long long)max_freq,
	       (unsigned long long)prios, (unsigned long long)ts0);

	if (alloc_bo(fd, 4096, &cmd_h, &cmd_iova, &cmd_map) < 0 ||
	    alloc_bo(fd, 4096, &dst_h, &dst_iova, &dst_map) < 0)
		return 1;

	cmd = (volatile uint32_t *)cmd_map;
	dst = (volatile uint32_t *)dst_map;
	dst[0] = 0x11111111u;
	dst[1] = 0x22222222u;

	cmd[0] = PKT7(CP_MEM_WRITE, 4);
	cmd[1] = (uint32_t)(dst_iova & 0xffffffffULL);
	cmd[2] = (uint32_t)(dst_iova >> 32);
	cmd[3] = 0xCAFEBABEu;
	cmd[4] = 0x51554553u;
	__sync_synchronize();

	if (ioctl(fd, DRM_IOCTL_MSM_SUBMITQUEUE_NEW, &sq) < 0) {
		perror("DRM_IOCTL_MSM_SUBMITQUEUE_NEW");
		return 1;
	}

	memset(bos, 0, sizeof(bos));
	bos[0].flags = MSM_SUBMIT_BO_READ;
	bos[0].handle = cmd_h;
	bos[0].presumed = cmd_iova;
	bos[1].flags = MSM_SUBMIT_BO_WRITE;
	bos[1].handle = dst_h;
	bos[1].presumed = dst_iova;

	memset(cmds, 0, sizeof(cmds));
	cmds[0].type = MSM_SUBMIT_CMD_BUF;
	cmds[0].submit_idx = 0;
	cmds[0].submit_offset = 0;
	cmds[0].size = 20;

	sub.flags = MSM_PIPE_3D0;
	sub.nr_bos = 2;
	sub.nr_cmds = 1;
	sub.bos = (uint64_t)(uintptr_t)bos;
	sub.cmds = (uint64_t)(uintptr_t)cmds;
	sub.queueid = sq.id;

	if (ioctl(fd, DRM_IOCTL_MSM_GEM_SUBMIT, &sub) < 0) {
		perror("DRM_IOCTL_MSM_GEM_SUBMIT");
		return 1;
	}
	printf("Submitted PKT7(CP_MEM_WRITE, 4) -> dst_iova=0x%llx queue=%u fence=%u\n",
	       (unsigned long long)dst_iova, sq.id, sub.fence);

	clock_gettime(CLOCK_MONOTONIC, &now);
	wf.fence = sub.fence;
	wf.queueid = sq.id;
	wf.timeout.tv_sec = now.tv_sec + 5;
	wf.timeout.tv_nsec = now.tv_nsec;
	if (ioctl(fd, DRM_IOCTL_MSM_WAIT_FENCE, &wf) < 0) {
		perror("DRM_IOCTL_MSM_WAIT_FENCE");
		return 1;
	}

	__sync_synchronize();
	get_param(fd, MSM_PARAM_TIMESTAMP, &ts1);
	printf("Readback: dst[0]=0x%08x dst[1]=0x%08x ts1=%llu (delta=%llu)\n",
	       dst[0], dst[1], (unsigned long long)ts1, (unsigned long long)(ts1 - ts0));

	if (dst[0] != 0xCAFEBABEu || dst[1] != 0x51554553u) {
		fprintf(stderr, "GPU_SUBMIT_FAIL: unexpected readback\n");
		return 1;
	}

	ioctl(fd, DRM_IOCTL_MSM_SUBMITQUEUE_CLOSE, &sq.id);
	printf("GPU_SUBMIT_PASS\n");
	return 0;
}
