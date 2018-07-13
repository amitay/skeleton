#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <openbmc_intf.h>
#include <openbmc.h>
#include <gpio.h>
#include <gpio_configs.h>

/* ------------------------------------------------------------------------- */
static const gchar* dbus_object_path = "/org/openbmc/control";
static const gchar* instance_name = "host0";
static const gchar* dbus_name = "org.openbmc.control.Host";

static GpioConfigs g_gpio_configs;

static GDBusObjectManagerServer *manager = NULL;

static GPIO* fsi_data;
static GPIO* fsi_clk;
static GPIO* fsi_enable;
static GPIO* cronus_sel;
static size_t num_optionals;
static GPIO* optionals;
static gboolean* optional_pols;

/* Bit bang patterns */

//putcfam pu 281c 30000000 -p0 (Primary Side Select)
static const char* primary = "000011111111110101111000111001100111111111111111111111111111101111111111";
//putcfam pu 281c B0000000 -p0
static const char* go = "000011111111110101111000111000100111111111111111111111111111101101111111";
//putcfam pu 0x281c 30900000 (Golden Side Select)
static const char* golden = "000011111111110101111000111001100111101101111111111111111111101001111111";

/* Setup attentions */
//putcfam pu 0x081C 20000000
static const char* attnA = "000011111111111101111110001001101111111111111111111111111111110001111111";
//putcfam pu 0x100D 40000000
static const char* attnB = "000011111111111011111100101001011111111111111111111111111111110001111111";
//putcfam pu 0x100B FFFFFFFF
static const char* attnC = "000011111111111011111101001000000000000000000000000000000000001011111111";



static gboolean
on_init(Control *control,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	control_complete_init(control,invocation);
	return TRUE;
}

int
fsi_bitbang(const char* pattern)
{
	int rc=GPIO_OK;
	int i;
	for(i=0;i<strlen(pattern);i++) {
		rc = gpio_writec(fsi_data,pattern[i]);
		if(rc!=GPIO_OK) { break; }
		rc = gpio_clock_cycle(fsi_clk,1);
		if(rc!=GPIO_OK) { break; }
	}
	return rc;
}

int
fsi_standby()
{
	int rc=GPIO_OK;
	rc = gpio_write(fsi_data,1);
	if(rc!=GPIO_OK) { return rc; }
	rc = gpio_clock_cycle(fsi_clk,5000);
	if(rc!=GPIO_OK) { return rc; }
	return rc;
}


static gboolean run_cmd(const char *path, char * const *argv)
{
	char output[1024];
	pid_t pid;
	int fd[2];
	ssize_t n;
	int ret, wstatus;

	ret = pipe(fd);
	if (ret == -1) {
		g_print("ERROR failed to create pipe to run cmd %s\n", path);
		return FALSE;
	}

	pid = fork();
	if (pid == -1) {
		g_print("ERROR failed to fork to run cmd %s\n", path);
		return FALSE;
	}

	if (pid == 0) {
		close(fd[0]);

		dup2(fd[1], 1);
		dup2(fd[1], 2);

		close(fd[1]);

		ret = execv(path, argv);
		exit(ret);
	}

	close(fd[1]);

	n = read(fd[0], output, sizeof(output));
	if (n > 0) {
		g_print("OUTPUT %s\n", output);
	}

	pid = wait(&wstatus);
	if (WIFEXITED(wstatus)) {
		ret = WEXITSTATUS(wstatus);
		if (ret != 0) {
			g_print("ERROR failed to run cmd %s, exit code %d\n", path, ret);
			return FALSE;
		}
	} else {
		g_print("ERROR process failed to run cmd %s\n", path);
		return FALSE;
	}

	return TRUE;
}

static gboolean
run_pdbg(unsigned int addr, unsigned int value)
{
	char *argv[8];
	char addr_str[16];
	char value_str[16];
	int ret;

	ret = snprintf(addr_str, sizeof(addr_str), "0x%x", addr);
	if (ret >= sizeof(addr_str)) {
		g_print("ERROR invalid address 0x%x\n", addr);
		return FALSE;
	}
	ret = snprintf(value_str, sizeof(value_str), "0x%x", value);
	if (ret >= sizeof(value_str)) {
		g_print("ERROR invalid value 0x%x\n", value);
		return FALSE;
	}

	argv[0] = "pdbg";
	argv[1] = "-b";
	argv[2] = "kernel";
	argv[3] = "-p0";
	argv[4] = "putcfam";
	argv[5] = addr_str;
	argv[6] = value_str;
	argv[7] = NULL;

	return run_cmd("/usr/bin/pdbg", argv);
}

static gboolean
on_boot(ControlHost *host,
		GDBusMethodInvocation *invocation,
		gpointer user_data)
{
	int rc = GPIO_OK;
	GDBusProxy *proxy;
	GError *error = NULL;
	GDBusConnection *connection =
		g_dbus_object_manager_server_get_connection(manager);
	gboolean ok;

	if (!(fsi_data && fsi_clk && fsi_enable && cronus_sel)) {
		g_print("ERROR invalid GPIO configuration, will not boot\n");
		return FALSE;
	}
	if(control_host_get_debug_mode(host)==1) {
		g_print("Enabling debug mode; not booting host\n");
		rc |= gpio_open(fsi_enable);
		rc |= gpio_open(cronus_sel);
		rc |= gpio_write(fsi_enable,1);
		rc |= gpio_write(cronus_sel,0);
		if(rc!=GPIO_OK) {
			g_print("ERROR enabling debug mode: %d\n",rc);
		}
		return TRUE;
	}
	g_print("Booting host\n");
	Control* control = object_get_control((Object*)user_data);
	control_host_complete_boot(host,invocation);
	do {
		rc = gpio_open(fsi_clk);
		rc |= gpio_open(fsi_data);
		rc |= gpio_open(fsi_enable);
		rc |= gpio_open(cronus_sel);
		for (size_t i = 0; i < num_optionals; ++i) {
			rc |= gpio_open(&optionals[i]);
		}
		if(rc!=GPIO_OK) { break; }

		//setup dc pins
		rc = gpio_write(cronus_sel,1);
		rc |= gpio_write(fsi_enable,1);
		rc |= gpio_write(fsi_clk,1);
		for (size_t i = 0; i < num_optionals; ++i) {
			rc |= gpio_write(&optionals[i], optional_pols[i]);
		}
		if(rc!=GPIO_OK) { break; }

		//data standy state
		rc = fsi_standby();

		//clear out pipes
		rc |= gpio_write(fsi_data,0);
		rc |= gpio_clock_cycle(fsi_clk,256);
		rc |= gpio_write(fsi_data,1);
		rc |= gpio_clock_cycle(fsi_clk,50);
		if(rc!=GPIO_OK) { break; }

		ok = run_pdbg(0x081C, 0x20000000);
		if (!ok) {
			g_print("ERROR pdbg attnA failed\n");
		}
		rc |= fsi_standby();

		ok = run_pdbg(0x100D, 0x40000000);
		if (!ok) {
			g_print("ERROR pdbg attnB failed\n");
		}
		rc |= fsi_standby();

		ok = run_pdbg(0x100B, 0xFFFFFFFF);
		if (!ok) {
			g_print("ERROR pdbg attnC failed\n");
		}
		rc |= fsi_standby();
		if(rc!=GPIO_OK) { break; }

		const gchar* flash_side = control_host_get_flash_side(host);
		g_print("Using %s side of the bios flash\n",flash_side);
		if(strcmp(flash_side,"primary")==0) {
			ok = run_pdbg(0x0281c, 0x30000000);
		} else if(strcmp(flash_side,"golden") == 0) {
			ok = run_pdbg(0x0281c, 0x30900000);
		} else {
			g_print("ERROR: Invalid flash side: %s\n",flash_side);
			rc = 0xff;
			ok = TRUE;
		}
		if (!ok) {
			g_print("ERROR pdbg %s failed\n", flash_side);
		}
		rc |= fsi_standby();
		if(rc!=GPIO_OK) { break; }

		ok = run_pdbg(0x0281c, 0xB0000000);
		if (!ok) {
			g_print("ERROR pdbg go failed\n");
		}
		rc = fsi_bitbang(go);

		rc |= gpio_write(fsi_data,1); /* Data standby state */
		rc |= gpio_clock_cycle(fsi_clk,2);

		rc |= gpio_write(fsi_clk,0); /* hold clk low for clock mux */
		rc |= gpio_write(fsi_enable,0);
		rc |= gpio_clock_cycle(fsi_clk,16);
		rc |= gpio_write(fsi_clk,0); /* Data standby state */

	} while(0);
	if(rc != GPIO_OK)
	{
		g_print("ERROR HostControl: GPIO sequence failed (rc=%d)\n",rc);
    }
	gpio_close(fsi_clk);
	gpio_close(fsi_data);
	gpio_close(fsi_enable);
	gpio_close(cronus_sel);
	for (size_t i = 0; i < num_optionals; ++i) {
		gpio_close(&optionals[i]);
	}

	control_host_emit_booted(host);

	return TRUE;
}

static void
on_bus_acquired(GDBusConnection *connection,
		const gchar *name,
		gpointer user_data)
{
	ObjectSkeleton *object;
	//g_print ("Acquired a message bus connection: %s\n",name);
	manager = g_dbus_object_manager_server_new(dbus_object_path);

	gchar *s;
	s = g_strdup_printf("%s/%s",dbus_object_path,instance_name);
	object = object_skeleton_new(s);
	g_free(s);

	ControlHost* control_host = control_host_skeleton_new();
	object_skeleton_set_control_host(object, control_host);
	g_object_unref(control_host);

	Control* control = control_skeleton_new();
	object_skeleton_set_control(object, control);
	g_object_unref(control);

	//define method callbacks here
	g_signal_connect(control_host,
			"handle-boot",
			G_CALLBACK(on_boot),
			object); /* user_data */
	g_signal_connect(control,
			"handle-init",
			G_CALLBACK(on_init),
			NULL); /* user_data */

	control_host_set_debug_mode(control_host,0);
	control_host_set_flash_side(control_host,"primary");

	/* Export the object (@manager takes its own reference to @object) */
	g_dbus_object_manager_server_set_connection(manager, connection);
	g_dbus_object_manager_server_export(manager, G_DBUS_OBJECT_SKELETON(object));
	g_object_unref(object);

	if(read_gpios(connection, &g_gpio_configs) != TRUE) {
		g_print("ERROR Hostctl: could not read GPIO configuration\n");
		return;
	}

	fsi_data = &g_gpio_configs.hostctl_gpio.fsi_data;
	fsi_clk = &g_gpio_configs.hostctl_gpio.fsi_clk;
	fsi_enable = &g_gpio_configs.hostctl_gpio.fsi_enable;
	cronus_sel = &g_gpio_configs.hostctl_gpio.cronus_sel;
	num_optionals = g_gpio_configs.hostctl_gpio.num_optionals;
	optionals = g_gpio_configs.hostctl_gpio.optionals;
	optional_pols = g_gpio_configs.hostctl_gpio.optional_pols;

	gpio_init(connection, fsi_data);
	gpio_init(connection, fsi_clk);
	gpio_init(connection, fsi_enable);
	gpio_init(connection, cronus_sel);
	for (int i = 0; i < num_optionals; ++i) {
		gpio_init(connection, &optionals[i]);
	}
}

static void
on_name_acquired(GDBusConnection *connection,
		const gchar *name,
		gpointer user_data)
{
	// g_print ("Acquired the name %s\n", name);
}

static void
on_name_lost(GDBusConnection *connection,
		const gchar *name,
		gpointer user_data)
{
	// g_print ("Lost the name %s\n", name);
	free_gpios(&g_gpio_configs);
}

gint
main(gint argc, gchar *argv[])
{
	GMainLoop *loop;
	cmdline cmd;
	cmd.argc = argc;
	cmd.argv = argv;

	guint id;
	loop = g_main_loop_new(NULL, FALSE);

	id = g_bus_own_name(DBUS_TYPE,
			dbus_name,
			G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
			G_BUS_NAME_OWNER_FLAGS_REPLACE,
			on_bus_acquired,
			on_name_acquired,
			on_name_lost,
			&cmd,
			NULL);

	g_main_loop_run(loop);

	g_bus_unown_name(id);
	g_main_loop_unref(loop);
	return 0;
}
