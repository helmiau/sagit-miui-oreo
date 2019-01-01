/*
 * Copyright (C) 2017, Sultanxda <sultanxda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "iosched-swch: " fmt

#include <linux/blkdev.h>
#include <linux/blk_types.h>
#include <linux/elevator.h>
#include <linux/cpufreq.h>
#include <linux/fb.h>

#define NOOP_IOSCHED "noop"
#define SLEEP_DELAY_MS (5000)

struct req_queue_data {
	struct list_head list;
	struct request_queue *queue;
	char prev_e[ELV_NAME_MAX];
	bool using_noop;
};

static struct workqueue_struct *susp_wq;
static struct work_struct restore_work;
static struct delayed_work sleep_work;

static DEFINE_SPINLOCK(init_lock);
static struct req_queue_data req_queues = {
	.list = LIST_HEAD_INIT(req_queues.list),
};

static void change_elevator(struct req_queue_data *r, bool use_noop)
{
	struct request_queue *q = r->queue;

	if (r->using_noop == use_noop)
		return;

	r->using_noop = use_noop;

	if (use_noop) {
		if(q->elevator) {
			strcpy(r->prev_e, q->elevator->type->elevator_name);
			elevator_change(q, NOOP_IOSCHED);
		}
	} else {
		if(q->elevator) {
			elevator_change(q, r->prev_e);
		}
	}
}

static void change_all_elevators(struct list_head *head, bool use_noop)
{
	struct req_queue_data *r;

	list_for_each_entry(r, head, list)
		change_elevator(r, use_noop);
}

static void change_min_freqs(bool sleep)
{
	if(sleep){
		cpufreq_set_min_freq(0, 300000);
		cpufreq_set_min_freq(4, 300000);
	}else{
		cpufreq_set_min_freq(0, 518400);
		cpufreq_set_min_freq(4, 806400);
	}
}

static int fb_notifier_callback(struct notifier_block *nb,
		unsigned long action, void *data)
{
	struct fb_event *evdata = data;
	int *blank = evdata->data;

	/* Parse framebuffer events as soon as they occur */
	if (action != FB_EVENT_BLANK)
		return NOTIFY_OK;

	switch (*blank) {
		case FB_BLANK_UNBLANK:
			if (delayed_work_pending(&sleep_work))
				cancel_delayed_work_sync(&sleep_work);

			queue_work(susp_wq, &restore_work);
			break;
		case FB_BLANK_POWERDOWN:
			queue_delayed_work(susp_wq, &sleep_work, msecs_to_jiffies(SLEEP_DELAY_MS));
			break;
	}

	return NOTIFY_OK;
}

static struct notifier_block fb_notifier_callback_nb = {
	.notifier_call = fb_notifier_callback,
};

static void restore_fn(struct work_struct *work)
{
	/*
	* Restore min freq
	*/
	change_min_freqs(false);

	/*
	* Switch to noop when the screen turns off.
	*/
	change_all_elevators(&req_queues.list, false);
}

static void sleep_fn(struct work_struct *work)
{
	/*
	* Switch back from noop to the original iosched
	* when the screen is turned on.
	*/
	change_all_elevators(&req_queues.list, true);

	/*
	* Set min freq to lowest when sleep
	*/
	change_min_freqs(true);
}

int init_iosched_switcher(struct request_queue *q)
{
	struct req_queue_data *r;

	r = kzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return -ENOMEM;

	r->queue = q;

	spin_lock(&init_lock);
	list_add(&r->list, &req_queues.list);
	spin_unlock(&init_lock);

	return 0;
}

static int iosched_switcher_core_init(void)
{
	susp_wq =
	    alloc_workqueue("state_susp_wq",
			    WQ_HIGHPRI | WQ_UNBOUND | WQ_MEM_RECLAIM, 0);
	INIT_WORK(&restore_work, restore_fn);
	INIT_DELAYED_WORK(&sleep_work, sleep_fn);
	fb_register_client(&fb_notifier_callback_nb);

	return 0;
}
late_initcall(iosched_switcher_core_init);

