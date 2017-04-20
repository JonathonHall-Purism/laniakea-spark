/*
 * Copyright (C) 2017 Matthias Klumpp <matthias@tenstral.net>
 *
 * Licensed under the GNU Lesser General Public License Version 3
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published by
 * the Free Software Foundation, either version 3 of the license, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this software.  If not, see <http://www.gnu.org/licenses/>.
 */

/**
 * SECTION: spark-engine
 * @short_description: Job runner and communicator.
 *
 * This class communicates with a Lighthouse server and schedules tasks.
 */

#include "config.h"
#include "spark-engine.h"

#include <json-glib/json-glib.h>
#include <czmq.h>

#include "spark-worker.h"

typedef struct _SparkEnginePrivate	SparkEnginePrivate;
struct _SparkEnginePrivate
{
	gchar *machine_id;   /* unique machine ID */
	gchar *machine_name; /* name of this machine */

	gchar *lighthouse_server; /* endpoint to connect to to receive jobs */
	guint max_jobs;          /* maximum number of tasks we can take */

	GPtrArray *workers;
	zsock_t *wsock;
	zsock_t *lhsock;

	GMainLoop *main_loop;
};

G_DEFINE_TYPE_WITH_PRIVATE (SparkEngine, spark_engine, G_TYPE_OBJECT)
#define GET_PRIVATE(o) (spark_engine_get_instance_private (o))

/* path to the global JSON configuration */
static const gchar *config_fname = "/etc/laniakea/spark.json";

/**
 * spark_engine_finalize:
 **/
static void
spark_engine_finalize (GObject *object)
{
	SparkEngine *engine = SPARK_ENGINE (object);
	SparkEnginePrivate *priv = GET_PRIVATE (engine);

	g_main_loop_unref (priv->main_loop);

	g_free (priv->machine_id);
	g_free (priv->machine_name);
	g_free (priv->lighthouse_server);
	g_ptr_array_unref (priv->workers);

	if (priv->lhsock != NULL)
		zsock_destroy (&priv->lhsock);
	zsock_destroy (&priv->wsock);

	G_OBJECT_CLASS (spark_engine_parent_class)->finalize (object);
}

/**
 * spark_engine_init:
 **/
static void
spark_engine_init (SparkEngine *engine)
{
	SparkEnginePrivate *priv = GET_PRIVATE (engine);

	priv->workers = g_ptr_array_new_with_free_func (g_object_unref);
	priv->main_loop = g_main_loop_new (NULL, FALSE);
	priv->max_jobs = 1;

	/* create internal socket for the worker processes to connect to */
	priv->wsock = zsock_new_pull ("inproc://workers");
	g_assert (priv->wsock);
}

/**
 * spark_engine_class_init:
 **/
static void
spark_engine_class_init (SparkEngineClass *klass)
{
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	object_class->finalize = spark_engine_finalize;
}

/**
 * spark_engine_route_messages:
 */
gboolean
spark_engine_route_messages (gpointer user_data)
{
	SparkEngine *engine = SPARK_ENGINE (user_data);
	SparkEnginePrivate *priv = GET_PRIVATE (engine);

	zstr_send (priv->lhsock, "TEST");

	return TRUE;
}

/**
 * spark_engine_load_config:
 */
static gboolean
spark_engine_load_config (SparkEngine *engine, GError **error)
{
	SparkEnginePrivate *priv = GET_PRIVATE (engine);
	g_autoptr(JsonParser) parser = NULL;
	JsonObject *root;

	/* fetch the machine ID first */
	if (!g_file_get_contents ("/etc/machine-id", &priv->machine_id, NULL, error))
		return FALSE;
	g_strdelimit (priv->machine_id, "\n", ' ');
	g_strstrip (priv->machine_id);

	parser = json_parser_new ();
	if (!json_parser_load_from_file (parser, config_fname, error))
		return FALSE;

	root = json_node_get_object (json_parser_get_root (parser));
	if (root == NULL) {
		g_set_error (error, SPARK_ENGINE_ERROR,
			     SPARK_ENGINE_ERROR_FAILED,
			     "The configuration in '%s' is not valid.", config_fname);
		return FALSE;
	}

	if (json_object_has_member (root, "MachineName")) {
		priv->machine_name = g_strdup (json_object_get_string_member (root, "MachineName"));
		g_strstrip (priv->machine_name);
	} else {
		/* we don't have a manually set machine name, take the hostname */
		if (!g_file_get_contents ("/etc/hostname", &priv->machine_name, NULL, error))
			return FALSE;
		g_strdelimit (priv->machine_name, "\n", ' ');
		g_strstrip (priv->machine_name);
	}

	priv->lighthouse_server = g_strdup (json_object_get_string_member (root, "LighthouseServer"));
	if (priv->lighthouse_server == NULL) {
		g_set_error (error, SPARK_ENGINE_ERROR,
			     SPARK_ENGINE_ERROR_FAILED,
			     "The configuration defines no Lighthouse server to connect to.");
		return FALSE;
	}

	if (json_object_has_member (root, "MaxJobs")) {
		priv->max_jobs = json_object_get_int_member (root, "MaxJobs");

		/* check values for sanity */
		if (priv->max_jobs <= 0 || priv->max_jobs > 100) {
			g_warning ("A number of %i jobs looks wrong. Resetting maximum job count to 1.", priv->max_jobs);
			priv->max_jobs = 1;
		}
	}

	return TRUE;
}

/**
 * spark_engine_run:
 */
gboolean
spark_engine_run (SparkEngine *engine, GError **error)
{
	SparkEnginePrivate *priv = GET_PRIVATE (engine);
	g_autoptr(GMainLoop) loop = NULL;
	guint i;

	if (!spark_engine_load_config (engine, error))
		return FALSE;

	if (priv->lhsock != NULL)
		zsock_destroy (&priv->lhsock);
	priv->lhsock = zsock_new_dealer (priv->lighthouse_server);
	if (priv->lhsock == NULL) {
		g_set_error (error, SPARK_ENGINE_ERROR,
			     SPARK_ENGINE_ERROR_FAILED,
			     "Unable to connect: %s",
			     g_strerror (errno));
		return FALSE;
	}

	/* new main loop for the master thread */
	loop = g_main_loop_new (NULL, FALSE);

	for (i = 0; i < priv->max_jobs; i++) {
		SparkWorker *worker;

		worker = spark_worker_new ();
		g_ptr_array_add (priv->workers, worker);
	}

	g_print ("Running on %s (%s), job capacity: %i\n", priv->machine_name, priv->machine_id, priv->max_jobs);

	g_idle_add (spark_engine_route_messages, engine);
	g_main_loop_run (loop);

	/* wait for workers to finish and clean up */
	for (i = 0; i < priv->workers->len; i++) {
		SparkWorker *worker = SPARK_WORKER (g_ptr_array_index (priv->workers, i));

		while (spark_worker_is_running (worker)) { sleep (1000); }
	}

	return TRUE;
}

/**
 * spark_engine_new:
 *
 * Creates a new #SparkEngine.
 *
 * Returns: (transfer full): a #SparkEngine
 *
 **/
SparkEngine*
spark_engine_new (void)
{
	SparkEngine *engine;
	engine = g_object_new (SPARK_TYPE_ENGINE, NULL);
	return SPARK_ENGINE (engine);
}

/**
 * spark_engine_error_quark:
 *
 * Return value: An error quark.
 **/
GQuark
spark_engine_error_quark (void)
{
	static GQuark quark = 0;
	if (!quark)
		quark = g_quark_from_static_string ("SparkEngineError");
	return quark;
}