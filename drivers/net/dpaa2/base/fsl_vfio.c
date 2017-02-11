/*-
 *   BSD LICENSE
 *
 *   Copyright (c) 2016 Freescale Semiconductor, Inc. All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Freescale Semiconductor, Inc nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <unistd.h>
#include <stdio.h>
#include <sys/types.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <sys/vfs.h>
#include <libgen.h>
#include <dirent.h>
#include <sys/eventfd.h>

#include <eal_filesystem.h>
#include <eal_private.h>
#include <rte_mbuf.h>
#include <rte_ethdev.h>
#include <rte_malloc.h>
#include <rte_memcpy.h>
#include <rte_string_fns.h>
#include <rte_cycles.h>
#include <rte_kvargs.h>
#include <rte_dev.h>
#include <rte_ethdev.h>

/* DPAA2 Global constants */
#include <dpaa2_logs.h>
#include <dpaa2_hw_pvt.h>

/* DPAA2 Base interface files */
#include <dpaa2_hw_dpbp.h>
#include <dpaa2_hw_dpni.h>
#include <dpaa2_hw_dpio.h>

#ifndef VFIO_MAX_GROUPS
#define VFIO_MAX_GROUPS 64
#endif

#define FSL_VFIO_LOG(level, fmt, args...) \
	RTE_LOG(level, EAL, "%s(): " fmt "\n", __func__, ##args)

/** Pathname of FSL-MC devices directory. */
#define SYSFS_FSL_MC_DEVICES "/sys/bus/fsl-mc/devices"

#define IRQ_SET_BUF_LEN  (sizeof(struct vfio_irq_set) + sizeof(int))

/* Number of VFIO containers & groups with in */
static struct fsl_vfio_group vfio_groups[VFIO_MAX_GRP];
static struct fsl_vfio_container vfio_containers[VFIO_MAX_CONTAINERS];
static int container_device_fd;
static uint32_t *msi_intr_vaddr;
void *(*mcp_ptr_list);
static uint32_t mcp_id;
static int is_dma_done;

static int vfio_connect_container(struct fsl_vfio_group *vfio_group)
{
	struct fsl_vfio_container *container;
	int i, fd, ret;

	/* Try connecting to vfio container if already created */
	for (i = 0; i < VFIO_MAX_CONTAINERS; i++) {
		container = &vfio_containers[i];
		if (!ioctl(vfio_group->fd, VFIO_GROUP_SET_CONTAINER,
			   &container->fd)) {
			FSL_VFIO_LOG(INFO, " Container pre-exists with"
				    " FD[0x%x] for this group",
				    container->fd);
			vfio_group->container = container;
			return 0;
		}
	}

	/* Opens main vfio file descriptor which represents the "container" */
	fd = vfio_get_container_fd();
	if (fd < 0) {
		FSL_VFIO_LOG(ERR, " Failed to open VFIO container");
		return -errno;
	}

	/* Check whether support for SMMU type IOMMU present or not */
	if (ioctl(fd, VFIO_CHECK_EXTENSION, VFIO_TYPE1_IOMMU)) {
		/* Connect group to container */
		ret = ioctl(vfio_group->fd, VFIO_GROUP_SET_CONTAINER, &fd);
		if (ret) {
			FSL_VFIO_LOG(ERR, " Failed to setup group container");
			close(fd);
			return -errno;
		}

		ret = ioctl(fd, VFIO_SET_IOMMU, VFIO_TYPE1_IOMMU);
		if (ret) {
			FSL_VFIO_LOG(ERR, " Failed to setup VFIO iommu");
			close(fd);
			return -errno;
		}
	} else {
		FSL_VFIO_LOG(ERR, " No supported IOMMU available");
		close(fd);
		return -EINVAL;
	}

	container = NULL;
	for (i = 0; i < VFIO_MAX_CONTAINERS; i++) {
		if (vfio_containers[i].used)
			continue;
		FSL_VFIO_LOG(DEBUG, " Unused container at index %d", i);
		container = &vfio_containers[i];
	}
	if (!container) {
		FSL_VFIO_LOG(ERR, " No free container found");
		close(fd);
		return -ENOMEM;
	}

	container->used = 1;
	container->fd = fd;
	container->group_list[container->index] = vfio_group;
	vfio_group->container = container;
	container->index++;
	return 0;
}

static int vfio_map_irq_region(struct fsl_vfio_group *group)
{
	int ret;
	unsigned long *vaddr = NULL;
	struct vfio_iommu_type1_dma_map map = {
		.argsz = sizeof(map),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
		.vaddr = 0x6030000,
		.iova = 0x6030000,
		.size = 0x1000,
	};

	vaddr = (unsigned long *)mmap(NULL, 0x1000, PROT_WRITE |
		PROT_READ, MAP_SHARED, container_device_fd, 0x6030000);
	if (vaddr == MAP_FAILED) {
		FSL_VFIO_LOG(ERR, " Unable to map region (errno = %d)", errno);
		return -errno;
	}

	msi_intr_vaddr = (uint32_t *)((char *)(vaddr) + 64);
	map.vaddr = (unsigned long)vaddr;
	ret = ioctl(group->container->fd, VFIO_IOMMU_MAP_DMA, &map);
	if (ret == 0)
		return 0;

	FSL_VFIO_LOG(ERR, " vfio_map_irq_region fails (errno = %d)", errno);
	return -errno;
}

int vfio_dmamap_mem_region(uint64_t vaddr,
			   uint64_t iova,
			   uint64_t size)
{
	struct fsl_vfio_group *group;
	struct vfio_iommu_type1_dma_map dma_map = {
		.argsz = sizeof(dma_map),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
	};

	dma_map.vaddr = vaddr;
	dma_map.size = size;
	dma_map.iova = iova;

	/* SET DMA MAP for IOMMU */
	group = &vfio_groups[0];
	if (ioctl(group->container->fd, VFIO_IOMMU_MAP_DMA, &dma_map)) {
		FSL_VFIO_LOG(ERR, "SWP: VFIO_IOMMU_MAP_DMA API"
			     " Error %d", errno);
		return -1;
	}
	return 0;
}

static int setup_dmamap(void)
{
	int ret;
	struct fsl_vfio_group *group;
	struct vfio_iommu_type1_dma_map dma_map = {
		.argsz = sizeof(struct vfio_iommu_type1_dma_map),
		.flags = VFIO_DMA_MAP_FLAG_READ | VFIO_DMA_MAP_FLAG_WRITE,
	};

	int i;
	const struct rte_memseg *memseg;

	/* SET DMA MAP for IOMMU */
	group = &vfio_groups[0];
	if (!group->container) {
		FSL_VFIO_LOG(ERR, " Container is not connected yet.");
		return -1;
	}

	for (i = 0; i < RTE_MAX_MEMSEG; i++) {
		memseg = rte_eal_get_physmem_layout();
		if (memseg == NULL) {
			FSL_VFIO_LOG(ERR, "Error Cannot get physical layout.");
			return -ENODEV;
		}

		if (memseg[i].addr == NULL && memseg[i].len == 0)
			break;

		dma_map.size = memseg[i].len;
		dma_map.vaddr = memseg[i].addr_64;
#ifdef RTE_LIBRTE_DPAA2_USE_PHYS_IOVA
		dma_map.iova = memseg[i].phys_addr;
#else
		dma_map.iova = dma_map.vaddr;
#endif
		FSL_VFIO_LOG(DEBUG, "-->Initial SHM Virtual ADDR %llX",
			     dma_map.vaddr);
		FSL_VFIO_LOG(DEBUG, "-----> DMA size 0x%llX\n", dma_map.size);
		ret = ioctl(group->container->fd, VFIO_IOMMU_MAP_DMA,
			    &dma_map);
		if (ret) {
			FSL_VFIO_LOG(ERR, " VFIO_IOMMU_MAP_DMA API Error %d",
				     errno);
			return ret;
		}
		FSL_VFIO_LOG(DEBUG, "-----> dma_map.vaddr = 0x%llX",
			     dma_map.vaddr);
	}

	/* TODO - This is a W.A. as VFIO currently does not add the mapping of
	 * the interrupt region to SMMU. This should be removed once the
	 * support is added in the Kernel.
	 */
	vfio_map_irq_region(group);

	return 0;
}

static int dpaa2_setup_vfio_grp(void)
{
	char path[PATH_MAX];
	char iommu_group_path[PATH_MAX + 1], *group_name;
	struct fsl_vfio_group *group = NULL;
	struct stat st;
	int groupid;
	int ret, len, i;
	char *container;
	struct vfio_group_status status = { .argsz = sizeof(status) };

	/* if already done once */
	if (container_device_fd)
		return 0;

	container = getenv("DPRC");

	if (container == NULL) {
		FSL_VFIO_LOG(ERR, " VFIO container not set in env DPRC");
		return -1;
	}
	RTE_LOG(INFO, PMD, "DPAA2: Processing Container = %s\n", container);
	snprintf(path, sizeof(path), "%s/%s", SYSFS_FSL_MC_DEVICES, container);

	/* Check whether fsl-mc container exists or not */
	FSL_VFIO_LOG(DEBUG, " container device path = %s", path);
	if (stat(path, &st) < 0) {
		FSL_VFIO_LOG(ERR, "vfio: Error (%d) getting FSL-MC device (%s)",
			     errno,  path);
		return -errno;
	}

	/* DPRC container exists. Now checkout the IOMMU Group */
	strncat(path, "/iommu_group", sizeof(path) - strlen(path) - 1);

	len = readlink(path, iommu_group_path, PATH_MAX);
	if (len == -1) {
		FSL_VFIO_LOG(ERR, " vfio: error no iommu_group for device");
		FSL_VFIO_LOG(ERR, "   %s: len = %d, errno = %d",
			     path, len, errno);
		return -errno;
	}

	iommu_group_path[len] = 0;
	group_name = basename(iommu_group_path);
	if (sscanf(group_name, "%d", &groupid) != 1) {
		FSL_VFIO_LOG(ERR, " VFIO error reading %s", path);
		return -errno;
	}

	FSL_VFIO_LOG(DEBUG, " VFIO iommu group id = %d", groupid);

	/* Check if group already exists */
	for (i = 0; i < VFIO_MAX_GRP; i++) {
		group = &vfio_groups[i];
		if (group->groupid == groupid) {
			FSL_VFIO_LOG(ERR, " groupid already exists %d",
				     groupid);
			return 0;
		}
	}

	/* Open the VFIO file corresponding to the IOMMU group */
	snprintf(path, sizeof(path), "/dev/vfio/%d", groupid);

	group->fd = open(path, O_RDWR);
	if (group->fd < 0) {
		FSL_VFIO_LOG(ERR, " VFIO error opening %s", path);
		return -1;
	}

	/* Test & Verify that group is VIABLE & AVAILABLE */
	if (ioctl(group->fd, VFIO_GROUP_GET_STATUS, &status)) {
		FSL_VFIO_LOG(ERR, " VFIO error getting group status");
		close(group->fd);
		return -1;
	}
	if (!(status.flags & VFIO_GROUP_FLAGS_VIABLE)) {
		FSL_VFIO_LOG(ERR, " VFIO group not viable");
		close(group->fd);
		return -1;
	}
	/* Since Group is VIABLE, Store the groupid */
	group->groupid = groupid;

	/* check if group does not have a container yet */
	if (!(status.flags & VFIO_GROUP_FLAGS_CONTAINER_SET)) {
		/* Now connect this IOMMU group to given container */
		if (vfio_connect_container(group)) {
			FSL_VFIO_LOG(ERR, "VFIO error connecting container with"
				    " groupid %d", groupid);
			close(group->fd);
			return -1;
		}
	}

	/* Get Device information */
	ret = ioctl(group->fd, VFIO_GROUP_GET_DEVICE_FD, container);
	if (ret < 0) {
		FSL_VFIO_LOG(ERR, " VFIO error getting device %s fd from group"
			    " %d", container, group->groupid);
		return ret;
	}
	container_device_fd = ret;
	FSL_VFIO_LOG(DEBUG, " VFIO Container FD is [0x%X]",
		     container_device_fd);

	return 0;
}

static int64_t vfio_map_mcp_obj(struct fsl_vfio_group *group, char *mcp_obj)
{
	int64_t v_addr = (int64_t)MAP_FAILED;
	int32_t ret, mc_fd;

	struct vfio_device_info d_info = { .argsz = sizeof(d_info) };
	struct vfio_region_info reg_info = { .argsz = sizeof(reg_info) };

	/* getting the mcp object's fd*/
	mc_fd = ioctl(group->fd, VFIO_GROUP_GET_DEVICE_FD, mcp_obj);
	if (mc_fd < 0) {
		FSL_VFIO_LOG(ERR, " VFIO error getting device %s fd from group"
			    " %d", mcp_obj, group->fd);
		return v_addr;
	}

	/* getting device info*/
	ret = ioctl(mc_fd, VFIO_DEVICE_GET_INFO, &d_info);
	if (ret < 0) {
		FSL_VFIO_LOG(ERR, "VFIO error getting DEVICE_INFO");
		goto MC_FAILURE;
	}

	/* getting device region info*/
	ret = ioctl(mc_fd, VFIO_DEVICE_GET_REGION_INFO, &reg_info);
	if (ret < 0) {
		FSL_VFIO_LOG(ERR, " VFIO error getting REGION_INFO");
		goto MC_FAILURE;
	}

	FSL_VFIO_LOG(DEBUG, " region offset = %llx  , region size = %llx",
		     reg_info.offset, reg_info.size);

	v_addr = (uint64_t)mmap(NULL, reg_info.size,
		PROT_WRITE | PROT_READ, MAP_SHARED,
		mc_fd, reg_info.offset);

MC_FAILURE:
	close(mc_fd);

	return v_addr;
}

int dpaa2_intr_enable(struct rte_intr_handle *intr_handle, int index)
{
	int len, ret;
	char irq_set_buf[IRQ_SET_BUF_LEN];
	struct vfio_irq_set *irq_set;
	int *fd_ptr;

	len = sizeof(irq_set_buf);

	irq_set = (struct vfio_irq_set *)irq_set_buf;
	irq_set->argsz = len;
	irq_set->count = 1;
	irq_set->flags =
		VFIO_IRQ_SET_DATA_EVENTFD | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = index;
	irq_set->start = 0;
	fd_ptr = (int *)&irq_set->data;
	*fd_ptr = intr_handle->fd;

	ret = ioctl(intr_handle->vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);

	if (ret) {
		RTE_LOG(ERR, EAL, "Error:dpaa2 SET IRQs fd=%d, err = %d(%s)\n",
			intr_handle->fd, errno, strerror(errno));
		return ret;
	}

	return ret;
}

int dpaa2_intr_unmask(struct rte_intr_handle *intr_handle, int index)
{
	int len, ret;
	char irq_set_buf[IRQ_SET_BUF_LEN];
	struct vfio_irq_set *irq_set;
	int *fd_ptr;

	len = sizeof(irq_set_buf);

	irq_set = (struct vfio_irq_set *)irq_set_buf;
	irq_set->argsz = len;
	irq_set->count = 1;
	irq_set->flags = VFIO_IRQ_SET_ACTION_UNMASK | VFIO_IRQ_SET_DATA_BOOL;
	irq_set->index = index;
	irq_set->start = 0;
	fd_ptr = (int *)&irq_set->data;
	*fd_ptr = 1;

	ret = ioctl(intr_handle->vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);

	if (ret) {
		RTE_LOG(ERR, EAL, "Error: dpaa2 SET IRQs fd=%d, err = %d(%s)\n",
			intr_handle->fd, errno, strerror(errno));
	}

	return ret;
}

int dpaa2_intr_disable(struct rte_intr_handle *intr_handle, int index)
{
	struct vfio_irq_set *irq_set;
	char irq_set_buf[IRQ_SET_BUF_LEN];
	int len, ret;

	len = sizeof(struct vfio_irq_set);

	irq_set = (struct vfio_irq_set *)irq_set_buf;
	irq_set->argsz = len;
	irq_set->flags = VFIO_IRQ_SET_DATA_NONE | VFIO_IRQ_SET_ACTION_TRIGGER;
	irq_set->index = index;
	irq_set->start = 0;
	irq_set->count = 0;

	ret = ioctl(intr_handle->vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);
	if (ret)
		RTE_LOG(ERR, EAL,
			"Error disabling dpaa2 interrupts for fd %d\n",
			intr_handle->fd);

	return ret;
}

int dpaa2_intr_mask(struct rte_intr_handle *intr_handle, int index)
{
	struct vfio_irq_set *irq_set;
	char irq_set_buf[IRQ_SET_BUF_LEN];
	int len, ret;
	int *fd_ptr;

	len = sizeof(struct vfio_irq_set);

	irq_set = (struct vfio_irq_set *)irq_set_buf;
	irq_set->argsz = len;
	irq_set->flags = VFIO_IRQ_SET_ACTION_MASK | VFIO_IRQ_SET_DATA_BOOL;
	irq_set->index = index;
	irq_set->start = 0;
	irq_set->count = 1;
	fd_ptr = (int *)&irq_set->data;
	*fd_ptr = 1;

	ret = ioctl(intr_handle->vfio_dev_fd, VFIO_DEVICE_SET_IRQS, irq_set);
	if (ret)
		RTE_LOG(ERR, EAL,
			"Error disabling dpaa2 interrupts for fd %d\n",
			intr_handle->fd);

	return ret;
}


/* set up interrupt support (but not enable interrupts) */
int
dpaa2_vfio_setup_intr(struct rte_intr_handle *intr_handle,
		      int vfio_dev_fd,
		      int num_irqs)
{
	int i, ret;

	/* start from MSI-X interrupt type */
	for (i = 0; i < num_irqs; i++) {
		struct vfio_irq_info irq_info = { .argsz = sizeof(irq_info) };
		int fd = -1;

		irq_info.index = i;

		ret = ioctl(vfio_dev_fd, VFIO_DEVICE_GET_IRQ_INFO, &irq_info);
		if (ret < 0) {
			FSL_VFIO_LOG(ERR, "  cannot get IRQ (%d) info, "
					"error %i (%s)", i, errno,
					strerror(errno));
			return -1;
		}

		FSL_VFIO_LOG(DEBUG, "IRQ Info (Count=%d, Flags=%d)",
			     irq_info.count, irq_info.flags);

		/* if this vector cannot be used with eventfd,
		 * fail if we explicitly
		 * specified interrupt type, otherwise continue
		 */
		if ((irq_info.flags & VFIO_IRQ_INFO_EVENTFD) == 0) {
			if (internal_config.vfio_intr_mode
			    != RTE_INTR_MODE_NONE) {
				FSL_VFIO_LOG(ERR, "  interrupt vector does not"
						 " support eventfd!\n");
				return -1;
			}
			continue;
		}

		/* set up an eventfd for interrupts */
		fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
		if (fd < 0) {
			FSL_VFIO_LOG(ERR, "  cannot set up eventfd, error %i"
					 "(%s)\n", errno, strerror(errno));
			return -1;
		}

		intr_handle->fd = fd;
		intr_handle->type = RTE_INTR_HANDLE_EXT;
		intr_handle->vfio_dev_fd = vfio_dev_fd;

		return 0;
	}

	/* if we're here, we haven't found a suitable interrupt vector */
	return -1;
}

/* Following function shall fetch total available list of MC devices
 * from VFIO container & populate private list of devices and other
 * data structures
 */
static int vfio_process_group_devices(void)
{
	struct fsl_vfio_device *vdev;
	struct vfio_device_info device_info = { .argsz = sizeof(device_info) };
	char *temp_obj, *object_type, *mcp_obj, *dev_name;
	int32_t object_id, i, dev_fd, ret;
	DIR *d;
	struct dirent *dir;
	char path[PATH_MAX];
	int64_t v_addr;
	int ndev_count;
	int dpio_count = 0, dpbp_count = 0;
	struct fsl_vfio_group *group = &vfio_groups[0];
	static int process_once;

	/* if already done once */
	if (process_once) {
		FSL_VFIO_LOG(DEBUG, "\n %s - Already scanned once - re-scan "
			    "not supported", __func__);
		return 0;
	}
	process_once = 0;

	sprintf(path, "/sys/kernel/iommu_groups/%d/devices", group->groupid);

	d = opendir(path);
	if (!d) {
		FSL_VFIO_LOG(ERR, "Unable to open directory %s", path);
		return -1;
	}

	/*Counting the number of devices in a group and getting the mcp ID*/
	ndev_count = 0;
	mcp_obj = NULL;
	while ((dir = readdir(d)) != NULL) {
		if (dir->d_type == DT_LNK) {
			ndev_count++;
			if (!strncmp("dpmcp", dir->d_name, 5)) {
				if (mcp_obj)
					free(mcp_obj);
				mcp_obj = malloc(sizeof(dir->d_name));
				if (!mcp_obj) {
					FSL_VFIO_LOG(ERR, "Unable to"
						    " allocate memory");
					closedir(d);
					return -ENOMEM;
				}
				strcpy(mcp_obj, dir->d_name);
				temp_obj = strtok(dir->d_name, ".");
				temp_obj = strtok(NULL, ".");
				sscanf(temp_obj, "%d", &mcp_id);
			}
		}
	}
	closedir(d);
	d = NULL;

	if (!mcp_obj) {
		FSL_VFIO_LOG(ERR, "DPAA2 MCP Object not Found");
		return -ENODEV;
	}
	RTE_LOG(INFO, PMD, "Total devices in container = %d, MCP ID = %d\n",
		     ndev_count, mcp_id);

	/* Allocate the memory depends upon number of objects in a group*/
	group->vfio_device = (struct fsl_vfio_device *)malloc(ndev_count *
			     sizeof(struct fsl_vfio_device));
	if (!(group->vfio_device)) {
		FSL_VFIO_LOG(ERR, " Unable to allocate memory\n");
		free(mcp_obj);
		return -ENOMEM;
	}

	/* Allocate memory for MC Portal list */
	mcp_ptr_list = malloc(sizeof(void *) * 1);
	if (!mcp_ptr_list) {
		FSL_VFIO_LOG(ERR, " Unable to allocate memory!");
		free(mcp_obj);
		goto FAILURE;
	}

	v_addr = vfio_map_mcp_obj(group, mcp_obj);
	free(mcp_obj);
	if (v_addr == (int64_t)MAP_FAILED) {
		FSL_VFIO_LOG(ERR, " Error mapping region (err = %d)", errno);
		goto FAILURE;
	}

	FSL_VFIO_LOG(DEBUG, " DPAA2 MC has VIR_ADD = 0x%ld", v_addr);

	mcp_ptr_list[0] = (void *)v_addr;

	d = opendir(path);
	if (!d) {
		FSL_VFIO_LOG(ERR, " Unable to open %s Directory", path);
		goto FAILURE;
	}

	i = 0;
	FSL_VFIO_LOG(DEBUG, "DPAA2 - Parsing devices:");
	/* Parsing each object and initiating them*/
	while ((dir = readdir(d)) != NULL) {
		if (dir->d_type != DT_LNK)
			continue;
		if (!strncmp("dprc", dir->d_name, 4) ||
		    !strncmp("dpmcp", dir->d_name, 5))
			continue;
		dev_name = malloc(sizeof(dir->d_name));
		if (!dev_name) {
			FSL_VFIO_LOG(ERR, " Unable to allocate memory");
			goto FAILURE;
		}
		strcpy(dev_name, dir->d_name);
		object_type = strtok(dir->d_name, ".");
		temp_obj = strtok(NULL, ".");
		sscanf(temp_obj, "%d", &object_id);
		FSL_VFIO_LOG(DEBUG, " - %s ", dev_name);

		/* getting the device fd*/
		dev_fd = ioctl(group->fd, VFIO_GROUP_GET_DEVICE_FD, dev_name);
		if (dev_fd < 0) {
			FSL_VFIO_LOG(ERR, " VFIO_GROUP_GET_DEVICE_FD error"
				    " Device fd: %s, Group: %d",
				    dev_name, group->fd);
			free(dev_name);
			goto FAILURE;
		}

		free(dev_name);
		vdev = &group->vfio_device[group->object_index++];
		vdev->fd = dev_fd;
		vdev->index = i;
		i++;
		/* Get Device inofrmation */
		if (ioctl(vdev->fd, VFIO_DEVICE_GET_INFO, &device_info)) {
			FSL_VFIO_LOG(ERR, "VFIO_DEVICE_FSL_MC_GET_INFO fail");
			goto FAILURE;
		}

		if (!strcmp(object_type, "dpni") ||
		    !strcmp(object_type, "dpseci")) {
			struct rte_pci_device *dev;

			dev = malloc(sizeof(struct rte_pci_device));
			if (dev == NULL) {
				FSL_VFIO_LOG(ERR, "device malloc failed");
				goto FAILURE;
			}

			memset(dev, 0, sizeof(*dev));
			/* store hw_id of dpni/dpseci device */
			dev->addr.devid = object_id;
			dev->id.vendor_id = FSL_VENDOR_ID;
			dev->id.device_id = (strcmp(object_type, "dpseci")) ?
				FSL_MC_DPNI_DEVID : FSL_MC_DPSECI_DEVID;
			dev->addr.function = dev->id.device_id;

			RTE_LOG(INFO, PMD, "DPAA2: Added [%s-%d]\n",
				object_type, object_id);
			/* Enable IRQ for DPNI devices */
			if (dev->id.device_id == FSL_MC_DPNI_DEVID)
				dpaa2_vfio_setup_intr(&dev->intr_handle,
						      vdev->fd,
						      device_info.num_irqs);

			TAILQ_INSERT_TAIL(&pci_device_list, dev, next);
		}

		if (!strcmp(object_type, "dpio")) {
			ret = dpaa2_create_dpio_device(vdev,
						       &device_info,
						       object_id);
			if (!ret)
				dpio_count++;
		}

		if (!strcmp(object_type, "dpbp")) {
			ret = dpaa2_create_dpbp_device(object_id);
			if (!ret)
				dpbp_count++;
		}
	}
	closedir(d);

	ret = dpaa2_affine_qbman_swp();
	if (ret) {
		FSL_VFIO_LOG(DEBUG, "Error in affining qbman swp %d", ret);
		FSL_VFIO_LOG(ERR, " DPAA2: Unable to initialize HW");
	}

	RTE_LOG(INFO, PMD, "DPAA2: Added dpbp_count = %d dpio_count=%d\n",
		     dpbp_count, dpio_count);
	return 0;

FAILURE:
	if (d)
		closedir(d);
	if (mcp_ptr_list) {
		free(mcp_ptr_list);
		mcp_ptr_list = NULL;
	}
	free(group->vfio_device);
	group->vfio_device = NULL;
	return -1;
}

/* Init the FSL-MC- LS2 EAL subsystem */
int
rte_eal_dpaa2_init(void)
{
#ifdef VFIO_PRESENT
	if (dpaa2_setup_vfio_grp()) {
		FSL_VFIO_LOG(DEBUG, "dpaa2_setup_vfio_grp");
		FSL_VFIO_LOG(ERR, "DPAA2: Unable to setup VFIO");
		return -1;
	}
	if (vfio_process_group_devices()) {
		FSL_VFIO_LOG(DEBUG, "vfio_process_group_devices");
		FSL_VFIO_LOG(ERR, "DPAA2: Unable to setup devices");
		return -1;
	}
	RTE_LOG(INFO, PMD, "DPAA2: Device setup completed\n");
#endif
	return 0;
}

int
rte_eal_dpaa2_dmamap(void)
{
	int ret = 0;

	/* Set up SMMU */
	if (!is_dma_done) {
		ret = setup_dmamap();
		if (ret) {
			RTE_LOG(ERR, PMD, "DPAA2: Unable to DMA Map devices");
			return ret;
		}
		is_dma_done = 1;
		RTE_LOG(INFO, PMD, "DPAA2: Devices DMA mapped successfully\n");
	} else {
		RTE_LOG(INFO, PMD, "DPAA2: Devices Already DMA mapped\n\n");
	}
	return ret;
}
