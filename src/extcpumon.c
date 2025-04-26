/*
 * extcpumon.c - A hwmon module for Intel Sandy Bridge or newer CPUs
 * that can monitor extra information.
 * 
 * Copyright (C) 2025 Ar Rakin <rakinar2@onesoftnet.eu.org>.
 * 
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define DRVNAME "extcpumon"
#define SENSOR_NAME DRVNAME
#define pr_fmt(fmt) DRVNAME ": " fmt

#include <linux/module.h>
#include <linux/platform_device.h>
#include <linux/init.h>
#include <linux/hwmon.h>
#include <asm/msr.h>
#include <asm/processor.h>

static umode_t extcpumon_is_visible(const void *data,
                            enum hwmon_sensor_types type,
                            u32 attr, int channel);
static int extcpumon_read(struct device *dev,
                           enum hwmon_sensor_types type,
                           u32 attr, int channel, long *val);
static int extcpumon_probe(struct platform_device *pdev);
static void remove_extcpumon(struct platform_device *pdev);

struct extcpumon_data {
    struct device *hwmon_dev;
    struct mutex update_lock;
};

struct cpu_core {
    unsigned int cpu_id;
    unsigned int core_id;
    bool is_available;
};

static struct cpu_core *cpus;
static size_t cpu_count = 0;
static size_t max_core_id = 0;

static struct platform_device *pdev;

static struct hwmon_ops extcpumon_ops = {
    .is_visible = &extcpumon_is_visible,
    .read = &extcpumon_read,
};

static struct hwmon_channel_info *extcpumon_channel_info[] = {
    & (struct hwmon_channel_info) {
		.type = hwmon_in,	
		.config = (u32 []) {
			0, 0	
		}
	},
    NULL
};

bool channel_info_initialized = false;

static struct hwmon_chip_info extcpumon_chip_info = {
    .ops = &extcpumon_ops,
    .info = (const struct hwmon_channel_info *const *) extcpumon_channel_info,
};

static struct platform_driver driver = {
    .driver = {
        .name = DRVNAME,
        .owner = THIS_MODULE,
    },
    .probe = &extcpumon_probe,
    .remove = &remove_extcpumon
};

static long cpu_read_vcore(unsigned int cpu)
{
    struct msr msr = {0};
    rdmsr_on_cpu(cpu, MSR_IA32_PERF_STATUS, &msr.l, &msr.h);
    return ((msr.h & 0xFFFF) * 1000) / 8192;
}

static umode_t extcpumon_is_visible(const void *data,
                            enum hwmon_sensor_types type,
                            u32 attr, int channel)
{
    if (type == hwmon_in && channel <= max_core_id) {
        if (!cpus[channel].is_available)
            return 0;

        switch (attr) {
            case hwmon_in_input:
            case hwmon_in_min:
            case hwmon_in_max:
                return 0444;
            
            default:
                return 0;
        }
    }

    return 0;
}

static int extcpumon_read(struct device *dev,
                           enum hwmon_sensor_types type,
                           u32 attr, int channel, long *val)
{
    long outval = -1;

    if (type == hwmon_in && channel <= max_core_id) {
        if (!cpus[channel].is_available)
            return -EINVAL;

        switch (attr) {
            case hwmon_in_input:
                outval = cpu_read_vcore(cpus[channel].cpu_id);
                break;

            case hwmon_in_min:
                outval = 500;
                break;

            case hwmon_in_max:
                outval = 1720;
                break;
        }
    }

    if (outval == -1)
        return -ENOTSUPP;

    struct extcpumon_data *data = dev_get_drvdata(dev);

    mutex_lock(&data->update_lock);
    *val = outval;
    mutex_unlock(&data->update_lock);

    return 0;
}


static int extcpumon_probe(struct platform_device *pdev)
{
    struct extcpumon_data *data;
    int ret;

    data = devm_kzalloc(&pdev->dev, sizeof(struct extcpumon_data), GFP_KERNEL);

    if (!data)
        return -ENOMEM;

    if (!channel_info_initialized) {
        u32 *config = kcalloc(max_core_id + 2, sizeof(u32), GFP_KERNEL);

        if (!config)
            return -ENOMEM;

        for (size_t i = 0; i <= max_core_id; i++) {
            config[i] = HWMON_I_INPUT | HWMON_I_MIN | HWMON_I_MAX;
        }

        config[max_core_id + 1] = 0;
        extcpumon_channel_info[0]->config = (const u32 *) config;
        channel_info_initialized = true;
    }

    mutex_init(&data->update_lock);
    data->hwmon_dev = hwmon_device_register_with_info(&pdev->dev, SENSOR_NAME,
                                                      data, &extcpumon_chip_info,
                                                      NULL);

    if (IS_ERR(data->hwmon_dev)) {
        ret = PTR_ERR(data->hwmon_dev);
        dev_err(&pdev->dev, "failed to register hwmon device: %d\n", ret);
        return ret;
    }

    platform_set_drvdata(pdev, data);
    return 0;
}

static void remove_extcpumon(struct platform_device *pdev)
{
    if (channel_info_initialized) {
        for (size_t i = 0; extcpumon_channel_info[i]; i++) {
            kfree((void *) extcpumon_channel_info[i]->config);
            extcpumon_channel_info[i]->config = NULL;
        }

        channel_info_initialized = false;
    }

    struct extcpumon_data *data = platform_get_drvdata(pdev);

    if (data) {
        hwmon_device_unregister(data->hwmon_dev);
    }
}

static int __init extcpumon_init(void)
{
    int cpu;
    size_t allocated_core_count = 16;
    cpumask_var_t cpus_online;

    if (!alloc_cpumask_var(&cpus_online, GFP_KERNEL)) {
        pr_err("failed to allocate cpumask\n");
        return -ENOMEM;
    }

    cpu_count = 0;
    cpus = kcalloc(allocated_core_count, sizeof (struct cpu_core), GFP_KERNEL);

    if (!cpus) {
        free_cpumask_var(cpus_online);
        pr_err("failed to allocate memory for cpu data\n");
        return -ENOMEM;
    }

    for_each_online_cpu(cpu) {
        if (allocated_core_count >= 1024) {
            pr_err("allocated count >= 1024: assertion failed");
            BUG();
        }

        reallocate:
        if (cpu_count >= allocated_core_count || max_core_id >= allocated_core_count) {
            struct cpu_core *new_cpus;
            size_t prev_allocated_core_count = allocated_core_count;
            
            allocated_core_count += 16;

            if (max_core_id > allocated_core_count) {
                allocated_core_count = max_core_id;
            }

            if (cpu_count > allocated_core_count) {
                allocated_core_count = cpu_count;
            }

            new_cpus = krealloc(cpus, allocated_core_count * sizeof (unsigned int), GFP_KERNEL);

            if (!new_cpus) {
                pr_err("failed to reallocate memory for cpu data\n");
                kfree(cpus);
                cpus = NULL;
                free_cpumask_var(cpus_online);
                return -ENOMEM;
            }

            for (size_t i = prev_allocated_core_count; i < allocated_core_count; i++) {
                new_cpus[i].is_available = false;
            }
            
            cpus = new_cpus;
        }

        unsigned int core_id = topology_core_id(cpu);

        if (core_id > max_core_id) {
            max_core_id = core_id;

            if (max_core_id >= allocated_core_count) {
                goto reallocate;
            }
        }

        if (cpumask_test_and_set_cpu(core_id, cpus_online)) {
            continue;
        }

        cpus[core_id].core_id = core_id;
        cpus[core_id].cpu_id = cpu;
        cpus[core_id].is_available = true;
        cpu_count++;
    }

    free_cpumask_var(cpus_online);

    pr_info("initializing with %zu CPU cores\n", cpu_count);

    for (unsigned int i = 0; i <= max_core_id; i++) {
        if (!cpus[i].is_available)
            continue;

        pr_info("CPU %u: Core ID %u\n", cpus[i].cpu_id, cpus[i].core_id);
    }
    
    pdev = platform_device_register_simple(DRVNAME, -1, NULL, 0);

    if (IS_ERR(pdev)) {
        pr_err("failed to register platform device\n");
        return PTR_ERR(pdev);
    }
    
    pr_info("initialized\n");
    return platform_driver_register(&driver);
}

static void __exit extcpumon_exit(void)
{
    platform_driver_unregister(&driver);
    platform_device_unregister(pdev);
    kfree(cpus);

    cpus = NULL;
    cpu_count = 0;
    max_core_id = 0;

    pr_info("unloaded\n");
}

module_init(extcpumon_init);
module_exit(extcpumon_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Ar Rakin <rakinar2@onesoftnet.eu.org>");
MODULE_DESCRIPTION("A hwmon module for Intel Sandy Bridge or newer CPUs that can monitor extra information");
MODULE_VERSION("1.0.0");