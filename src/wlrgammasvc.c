#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <glib.h>
#include <glib-unix.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <wayland-client-protocol.h>
#include <wayland-client.h>
#include "gen/wlr-gamma-control-unstable-v1-client-protocol.h"
#include "gen/wlrgammasvcbus.h"
#include "color_math.h"

struct output
{
	uint32_t global_name;
	struct wl_output *wl_output;
	struct zwlr_gamma_control_v1 *gamma_control;
	uint32_t ramp_size; // 0 until the compositor sends gamma_size
	struct wl_list link;
};

static struct wl_list outputs;
static struct zwlr_gamma_control_manager_v1 *gamma_control_manager = NULL;
static struct wl_display *display;
static double current_temp = 6500;
static double current_brightness = 1.0;
// NVIDIA drops a gamma table committed while the previous one is still uploading
static gint64 min_interval_us;
static gint64 last_apply_us = 0;
static guint pending_apply = 0;

static int create_anonymous_file(off_t size)
{
	char template[] = "/tmp/wlroots-shared-XXXXXX";
	int fd = mkstemp(template);
	if (fd < 0)
	{
		return -1;
	}

	int ret;
	do
	{
		errno = 0;
		ret = ftruncate(fd, size);
	} while (errno == EINTR);
	if (ret < 0)
	{
		close(fd);
		return -1;
	}

	unlink(template);
	return fd;
}

static int create_gamma_table(uint32_t ramp_size, uint16_t **table)
{
	size_t table_size = ramp_size * 3 * sizeof(uint16_t);
	int fd = create_anonymous_file(table_size);
	if (fd < 0)
	{
		fprintf(stderr, "failed to create anonymous file\n");
		return -1;
	}

	void *data =
		mmap(NULL, table_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED)
	{
		fprintf(stderr, "failed to mmap()\n");
		close(fd);
		return -1;
	}

	*table = data;
	return fd;
}

static void fill_gamma_table(uint16_t *table, uint32_t ramp_size,
							 double brightness, int temp);

static void apply_output(struct output *output)
{
	uint16_t *table;
	int fd = create_gamma_table(output->ramp_size, &table);
	if (fd < 0)
	{
		exit(EXIT_FAILURE);
	}
	fill_gamma_table(table, output->ramp_size, current_brightness, current_temp);
	zwlr_gamma_control_v1_set_gamma(output->gamma_control, fd);
	wl_display_flush(display);
	munmap(table, output->ramp_size * 3 * sizeof(uint16_t));
	close(fd);
}

static void remove_output(struct output *output)
{
	if (output->gamma_control != NULL)
	{
		zwlr_gamma_control_v1_destroy(output->gamma_control);
	}
	wl_output_destroy(output->wl_output);
	wl_list_remove(&output->link);
	free(output);
}

static void gamma_control_handle_gamma_size(void *data,
											struct zwlr_gamma_control_v1 *gamma_control, uint32_t ramp_size)
{
	struct output *output = data;
	output->ramp_size = ramp_size;
	apply_output(output);
}

static void gamma_control_handle_failed(void *data,
										struct zwlr_gamma_control_v1 *gamma_control)
{
	struct output *output = data;
	fprintf(stderr, "gamma control failed for output %u, dropping it\n", output->global_name);
	remove_output(output);
}

static const struct zwlr_gamma_control_v1_listener gamma_control_listener = {
	.gamma_size = gamma_control_handle_gamma_size,
	.failed = gamma_control_handle_failed,
};

static void attach_gamma_control(struct output *output)
{
	output->gamma_control = zwlr_gamma_control_manager_v1_get_gamma_control(
		gamma_control_manager, output->wl_output);
	zwlr_gamma_control_v1_add_listener(output->gamma_control,
									   &gamma_control_listener, output);
}

static void registry_handle_global(void *data, struct wl_registry *registry,
								   uint32_t name, const char *interface, uint32_t version)
{
	if (strcmp(interface, wl_output_interface.name) == 0)
	{
		struct output *output = calloc(1, sizeof(struct output));
		output->global_name = name;
		output->wl_output = wl_registry_bind(registry, name,
											 &wl_output_interface, 1);
		wl_list_insert(&outputs, &output->link);
		if (gamma_control_manager != NULL)
		{
			attach_gamma_control(output);
			wl_display_flush(display);
		}
	}
	else if (strcmp(interface,
					zwlr_gamma_control_manager_v1_interface.name) == 0)
	{
		gamma_control_manager = wl_registry_bind(registry, name,
												 &zwlr_gamma_control_manager_v1_interface, 1);
	}
}

static void registry_handle_global_remove(void *data,
										  struct wl_registry *registry, uint32_t name)
{
	struct output *output, *tmp;
	wl_list_for_each_safe(output, tmp, &outputs, link)
	{
		if (output->global_name == name)
		{
			remove_output(output);
		}
	}
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_handle_global,
	.global_remove = registry_handle_global_remove,
};

static void fill_gamma_table(uint16_t *table, uint32_t ramp_size,
							 double brightness, int temp)
{
	double rw, gw, bw;
	calc_whitepoint(temp, &rw, &gw, &bw);
	uint16_t *r = table;
	uint16_t *g = table + ramp_size;
	uint16_t *b = table + 2 * ramp_size;
	for (uint32_t i = 0; i < ramp_size; ++i)
	{
		double val = (double)i / (ramp_size - 1);

		// We never want to get brighter, but we also want to dim bright colors a
		// little bit more than dark colors.
		val = fmin(pow(val, 0.9) * brightness, val);
		if (val > 1.0)
		{
			val = 1.0;
		}
		else if (val < 0.0)
		{
			val = 0.0;
		}
		r[i] = (uint16_t)(UINT16_MAX * val * rw);
		g[i] = (uint16_t)(UINT16_MAX * val * gw);
		b[i] = (uint16_t)(UINT16_MAX * val * bw);
	}
}

static gboolean
apply_all(gpointer user_data)
{
	pending_apply = 0;
	struct output *output;
	wl_list_for_each(output, &outputs, link)
	{
		if (output->ramp_size > 0) // the rest get it on gamma_size
		{
			apply_output(output);
		}
	}
	last_apply_us = g_get_monotonic_time();
	return G_SOURCE_REMOVE;
}

static void
set_gamma(double new_brightness, int new_temp)
{
	if (new_brightness > 1.0)
	{
		new_brightness = 1.0;
	}
	if (new_brightness < 0.0)
	{
		new_brightness = 0.0;
	}

	if (new_temp > 16000)
	{
		new_temp = 16000;
	}
	if (new_temp < 1600)
	{
		new_temp = 1600;
	}

	current_brightness = new_brightness;
	current_temp = new_temp;

	if (pending_apply != 0)
	{
		return; // the scheduled apply picks up the latest values
	}
	gint64 wait_us = last_apply_us + min_interval_us - g_get_monotonic_time();
	if (wait_us <= 0)
	{
		apply_all(NULL);
	}
	else
	{
		pending_apply = g_timeout_add((wait_us + 999) / 1000, apply_all, NULL);
	}
}

static gboolean
on_handle_increase_brightness(WlrGammaSvcBusBrightness *interface, GDBusMethodInvocation *invocation,
							  const gdouble value, gpointer user_data)
{
	set_gamma(current_brightness + value, current_temp);
	wlr_gamma_svc_bus_brightness_complete_increase(interface, invocation, current_brightness);
	return TRUE;
}

static gboolean
on_handle_decrease_brightness(WlrGammaSvcBusBrightness *interface, GDBusMethodInvocation *invocation,
							  const gdouble value, gpointer user_data)
{
	set_gamma(current_brightness - value, current_temp);
	wlr_gamma_svc_bus_brightness_complete_decrease(interface, invocation, current_brightness);
	return TRUE;
}

static gboolean
on_handle_set_brightness(WlrGammaSvcBusBrightness *interface, GDBusMethodInvocation *invocation,
						 const gdouble value, gpointer user_data)
{
	set_gamma(value, current_temp);
	wlr_gamma_svc_bus_brightness_complete_set(interface, invocation, current_brightness);
	return TRUE;
}

static gboolean
on_handle_get_brightness(WlrGammaSvcBusBrightness *interface, GDBusMethodInvocation *invocation, gpointer user_data)
{
	wlr_gamma_svc_bus_brightness_complete_get(interface, invocation, current_brightness);
	return TRUE;
}

static gboolean
on_handle_increase_temperature(WlrGammaSvcBusTemperature *interface, GDBusMethodInvocation *invocation,
							   const gint value, gpointer user_data)
{
	set_gamma(current_brightness, current_temp + value);
	wlr_gamma_svc_bus_temperature_complete_increase(interface, invocation, current_temp);
	return TRUE;
}

static gboolean
on_handle_decrease_temperature(WlrGammaSvcBusTemperature *interface, GDBusMethodInvocation *invocation,
							   const gint value, gpointer user_data)
{
	set_gamma(current_brightness, current_temp - value);
	wlr_gamma_svc_bus_temperature_complete_decrease(interface, invocation, current_temp);
	return TRUE;
}

static gboolean
on_handle_set_temperature(WlrGammaSvcBusTemperature *interface, GDBusMethodInvocation *invocation,
						  const gint value, gpointer user_data)
{
	set_gamma(current_brightness, value);
	wlr_gamma_svc_bus_temperature_complete_set(interface, invocation, current_temp);
	return TRUE;
}

static gboolean
on_handle_get_temperature(WlrGammaSvcBusTemperature *interface, GDBusMethodInvocation *invocation, gpointer user_data)
{
	wlr_gamma_svc_bus_temperature_complete_get(interface, invocation, current_temp);
	return TRUE;
}

static gboolean
on_handle_set_gamma(WlrGammaSvcBusGamma *interface, GDBusMethodInvocation *invocation,
					const gint temperature, const gdouble brightness, gpointer user_data)
{
	set_gamma(brightness, temperature);
	wlr_gamma_svc_bus_gamma_complete_set(interface, invocation, current_temp, current_brightness);
	return TRUE;
}

static gboolean
on_handle_adjust_gamma(WlrGammaSvcBusGamma *interface, GDBusMethodInvocation *invocation,
					   const gint temperature_delta, const gdouble brightness_delta, gpointer user_data)
{
	set_gamma(current_brightness + brightness_delta, current_temp + temperature_delta);
	wlr_gamma_svc_bus_gamma_complete_adjust(interface, invocation, current_temp, current_brightness);
	return TRUE;
}

static void
on_name_lost(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
	fprintf(stderr, "couldn't own or lost bus name %s\n", name);
	exit(EXIT_FAILURE);
}

static gboolean
on_wayland_event(gint fd, GIOCondition condition, gpointer user_data)
{
	if ((condition & (G_IO_HUP | G_IO_ERR)) || wl_display_dispatch(display) < 0)
	{
		fprintf(stderr, "lost the wayland connection\n");
		exit(EXIT_FAILURE);
	}
	return G_SOURCE_CONTINUE;
}

static void
on_name_acquired(GDBusConnection *connection, const gchar *name, gpointer user_data)
{
	WlrGammaSvcBusBrightness *brightness_interface;
	WlrGammaSvcBusTemperature *temperature_interface;
	WlrGammaSvcBusGamma *gamma_interface;
	GError *error;

	brightness_interface = wlr_gamma_svc_bus_brightness_skeleton_new();
	g_signal_connect(brightness_interface, "handle-get", G_CALLBACK(on_handle_get_brightness), NULL);
	g_signal_connect(brightness_interface, "handle-set", G_CALLBACK(on_handle_set_brightness), NULL);
	g_signal_connect(brightness_interface, "handle-increase", G_CALLBACK(on_handle_increase_brightness), NULL);
	g_signal_connect(brightness_interface, "handle-decrease", G_CALLBACK(on_handle_decrease_brightness), NULL);
	error = NULL;
	!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(brightness_interface), connection, "/net/zoidplex/wlr_gamma_service", &error);

	temperature_interface = wlr_gamma_svc_bus_temperature_skeleton_new();
	g_signal_connect(temperature_interface, "handle-get", G_CALLBACK(on_handle_get_temperature), NULL);
	g_signal_connect(temperature_interface, "handle-set", G_CALLBACK(on_handle_set_temperature), NULL);
	g_signal_connect(temperature_interface, "handle-increase", G_CALLBACK(on_handle_increase_temperature), NULL);
	g_signal_connect(temperature_interface, "handle-decrease", G_CALLBACK(on_handle_decrease_temperature), NULL);
	error = NULL;
	!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(temperature_interface), connection, "/net/zoidplex/wlr_gamma_service", &error);

	gamma_interface = wlr_gamma_svc_bus_gamma_skeleton_new();
	g_signal_connect(gamma_interface, "handle-set", G_CALLBACK(on_handle_set_gamma), NULL);
	g_signal_connect(gamma_interface, "handle-adjust", G_CALLBACK(on_handle_adjust_gamma), NULL);
	error = NULL;
	!g_dbus_interface_skeleton_export(G_DBUS_INTERFACE_SKELETON(gamma_interface), connection, "/net/zoidplex/wlr_gamma_service", &error);
}

int main(int argc, char *argv[])
{
	char *end;
	long min_interval_ms;
	if (argc != 3 || strcmp(argv[1], "--min-interval-ms") != 0 ||
		(min_interval_ms = strtol(argv[2], &end, 10)) < 0 || *end != '\0' || end == argv[2])
	{
		fprintf(stderr, "usage: %s --min-interval-ms <ms>\n", argv[0]);
		return EXIT_FAILURE;
	}
	min_interval_us = (gint64)min_interval_ms * 1000;

	wl_list_init(&outputs);

	display = wl_display_connect(NULL);
	if (display == NULL)
	{
		fprintf(stderr, "failed to create display\n");
		return -1;
	}

	struct wl_registry *registry = wl_display_get_registry(display);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_dispatch(display);
	wl_display_roundtrip(display);

	if (gamma_control_manager == NULL)
	{
		fprintf(stderr,
				"compositor doesn't support wlr-gamma-control-unstable-v1\n");
		return EXIT_FAILURE;
	}

	struct output *output;
	wl_list_for_each(output, &outputs, link)
	{
		if (output->gamma_control == NULL)
		{
			attach_gamma_control(output);
		}
	}
	wl_display_roundtrip(display);
	g_unix_fd_add(wl_display_get_fd(display), G_IO_IN | G_IO_HUP | G_IO_ERR, on_wayland_event, NULL);

	GMainLoop *loop;
	loop = g_main_loop_new(NULL, FALSE);

	g_bus_own_name(G_BUS_TYPE_SESSION, "net.zoidplex.wlr_gamma_service", G_BUS_NAME_OWNER_FLAGS_NONE, NULL,
				   on_name_acquired, on_name_lost, NULL, NULL);

	g_main_loop_run(loop);

	return EXIT_SUCCESS;
}
