/*
 * This file is part of playerctl.
 *
 * playerctl is free software: you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 *
 * playerctl is distributed in the hope that it will be useful, but WITHOUT ANY
 * WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public License for
 * more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with playerctl If not, see <http://www.gnu.org/licenses/>.
 *
 * Copyright © 2014, Tony Crisci and contributors.
 */

#include <stdbool.h>
#include <gio/gio.h>
#include <locale.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <inttypes.h>
#include "playerctl.h"
#include "playerctl-common.h"
#include "playerctl-formatter.h"

#define LENGTH(array) (sizeof array / sizeof array[0])

G_DEFINE_QUARK(playerctl-cli-error-quark, playerctl_cli_error);

/* A comma separated list of players to control. */
static gchar *player_arg = NULL;
/* A comma separated list of players to ignore. */
static gchar *ignore_player_arg = NULL;
/* If true, control all available media players */
static gboolean select_all_players;
/* If true, list all available players' names and exit. */
static gboolean list_all_players_and_exit;
/* If true, print the version and exit. */
static gboolean print_version_and_exit;
/* The commands passed on the command line, filled in via G_OPTION_REMAINING. */
static gchar **command = NULL;
/* A format string for printing properties and metadata */
static gchar *format_string_arg = NULL;
/* The formatter for the format string argument if present */
PlayerctlFormatter *formatter = NULL;
/* Block and follow the command */
static gboolean follow = FALSE;
/* The main loop for the follow command */
static GMainLoop *main_loop = NULL;
/* A list of the players currently being followed */
GList *followed_players = NULL;
/* The last output printed by the cli */
static gchar *last_output = NULL;

/* forward definitions */
static void followed_players_execute_command(GError **error);

/*
 * Sometimes players may notify metadata when nothing we care about has
 * changed, so we have this to avoid printing duplicate lines in follow
 * mode. Prints a newline if output is NULL which denotes that the property has
 * been cleared. Only use this in follow mode.
 *
 * This consumes the output string.
 */
static void cli_print_output(gchar *output) {
    if (output == NULL && last_output == NULL) {
        return;
    }

    if (output == NULL) {
        output = g_strdup("\n");
    }

    if (g_strcmp0(output, last_output) == 0) {
        g_free(output);
        return;
    }

    printf("%s", output);
    last_output = output;
}

struct playercmd_args {
    gchar **argv;
    gint argc;
};

/* Arguments given to the player for the follow command */
static struct playercmd_args *playercmd_args = NULL;

static struct playercmd_args *playercmd_args_create(gchar **argv, gint argc) {
    struct playercmd_args *user_data = calloc(1, sizeof(struct playercmd_args));
    user_data->argc = argc;
    user_data->argv = g_strdupv(argv);
    return user_data;
}

static void playercmd_args_destroy(struct playercmd_args *data) {
    if (data == NULL) {
        return;
    }

    g_strfreev(data->argv);
    free(data);

    return;
}

static gchar *get_metadata_formatted(PlayerctlPlayer *player, GError **error) {
    GError *tmp_error = NULL;
    GVariant *metadata = NULL;

    g_return_val_if_fail(formatter != NULL, NULL);

    g_object_get(player, "metadata", &metadata, NULL);
    if (metadata == NULL) {
        return NULL;
    }

    if (g_variant_n_children(metadata) == 0) {
        g_variant_unref(metadata);
        return NULL;
    }

    GVariantDict *metadata_dict =
        playerctl_formatter_default_template_context(formatter, player, metadata);

    gchar *result = playerctl_formatter_expand_format(formatter, metadata_dict, &tmp_error);
    if (tmp_error) {
        g_variant_unref(metadata);
        g_variant_dict_unref(metadata_dict);
        g_propagate_error(error, tmp_error);
        return NULL;
    }

    g_variant_unref(metadata);
    g_variant_dict_unref(metadata_dict);

    return result;
}

static gboolean playercmd_play(PlayerctlPlayer *player, gchar **argv, gint argc,
                               gchar **output, GError **error) {
    GError *tmp_error = NULL;

    gboolean can_play = FALSE;
    g_object_get(player, "can-play", &can_play, NULL);

    if (!can_play) {
        return FALSE;
    }

    playerctl_player_play(player, &tmp_error);
    if (tmp_error) {
        g_propagate_error(error, tmp_error);
        return FALSE;
    }
    return TRUE;
}

static gboolean playercmd_pause(PlayerctlPlayer *player, gchar **argv, gint argc,
                                gchar **output, GError **error) {
    GError *tmp_error = NULL;

    gboolean can_pause = FALSE;
    g_object_get(player, "can-pause", &can_pause, NULL);

    if (!can_pause) {
        return FALSE;
    }

    playerctl_player_pause(player, &tmp_error);
    if (tmp_error) {
        g_propagate_error(error, tmp_error);
        return FALSE;
    }
    return TRUE;
}

static gboolean playercmd_play_pause(PlayerctlPlayer *player, gchar **argv, gint argc,
                                     gchar **output, GError **error) {
    GError *tmp_error = NULL;

    gboolean can_play = FALSE;
    g_object_get(player, "can-play", &can_play, NULL);

    if (!can_play) {
        return FALSE;
    }

    playerctl_player_play_pause(player, &tmp_error);
    if (tmp_error) {
        g_propagate_error(error, tmp_error);
        return FALSE;
    }
    return TRUE;
}

static gboolean playercmd_stop(PlayerctlPlayer *player, gchar **argv, gint argc,
                               gchar **output, GError **error) {
    GError *tmp_error = NULL;

    // XXX there is no CanStop propery on the mpris player. CanPlay is supposed
    // to indicate whether there is a current track. If there is no current
    // track, then I assume the player cannot stop.
    gboolean can_play = FALSE;
    g_object_get(player, "can-play", &can_play, NULL);

    if (!can_play) {
        return FALSE;
    }

    playerctl_player_stop(player, &tmp_error);
    if (tmp_error) {
        g_propagate_error(error, tmp_error);
        return FALSE;
    }
    return TRUE;

}

static gboolean playercmd_next(PlayerctlPlayer *player, gchar **argv, gint argc,
                               gchar **output, GError **error) {
    GError *tmp_error = NULL;

    gboolean can_go_next = FALSE;
    g_object_get(player, "can-go-next", &can_go_next, NULL);

    if (!can_go_next) {
        return FALSE;
    }

    playerctl_player_next(player, &tmp_error);
    if (tmp_error) {
        g_propagate_error(error, tmp_error);
        return FALSE;
    }
    return TRUE;
}

static gboolean playercmd_previous(PlayerctlPlayer *player, gchar **argv, gint argc,
                                   gchar **output, GError **error) {
    GError *tmp_error = NULL;

    gboolean can_go_previous = FALSE;
    g_object_get(player, "can-go-previous", &can_go_previous, NULL);

    if (!can_go_previous) {
        return FALSE;
    }

    playerctl_player_previous(player, &tmp_error);
    if (tmp_error) {
        g_propagate_error(error, tmp_error);
        return FALSE;
    }
    return TRUE;

}

static gboolean playercmd_open(PlayerctlPlayer *player, gchar **argv, gint argc,
                               gchar **output, GError **error) {
    const gchar *uri = *argv;
    GError *tmp_error = NULL;

    if (uri) {
        playerctl_player_open(player,
                              g_file_get_uri(g_file_new_for_commandline_arg(uri)),
                              &tmp_error);
        if (tmp_error != NULL) {
            g_propagate_error(error, tmp_error);
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean playercmd_position(PlayerctlPlayer *player, gchar **argv, gint argc,
                                   gchar **output, GError **error) {
    const gchar *position = argv[1];
    gint64 offset;
    GError *tmp_error = NULL;

    if (position) {
        if (format_string_arg != NULL) {
            g_set_error(error, playerctl_cli_error_quark(), 1,
                    "format strings are not supported on command functions.");
            return FALSE;
        }

        char *endptr = NULL;
        offset = 1000000.0 * strtod(position, &endptr);

        if (position == endptr) {
            g_set_error(error, playerctl_cli_error_quark(), 1,
                        "Could not parse position as a number: %s\n", position);
            return FALSE;
        }

        gboolean can_seek = FALSE;
        g_object_get(player, "can-seek", &can_seek, NULL);
        if (!can_seek) {
            return FALSE;
        }

        size_t last = strlen(position) - 1;
        if (position[last] == '+' || position[last] == '-') {
            if (position[last] == '-') {
                offset *= -1;
            }

            playerctl_player_seek(player, offset, &tmp_error);
            if (tmp_error != NULL) {
                g_propagate_error(error, tmp_error);
                return FALSE;
            }
        } else {
            playerctl_player_set_position(player, offset, &tmp_error);
            if (tmp_error != NULL) {
                g_propagate_error(error, tmp_error);
                return FALSE;
            }
        }
    } else {
        if (formatter != NULL) {
            GVariantDict *context =
                playerctl_formatter_default_template_context(formatter, player, NULL);
            gchar *formatted =
                playerctl_formatter_expand_format(formatter, context, &tmp_error);
            if (tmp_error != NULL) {
                g_propagate_error(error, tmp_error);
                g_variant_dict_unref(context);
                return FALSE;
            }

            *output = g_strdup_printf("%s\n", formatted);

            g_free(formatted);
            g_variant_dict_unref(context);
        } else {
            g_object_get(player, "position", &offset, NULL);
            *output = g_strdup_printf("%f\n", (double)offset / 1000000.0);
        }
    }

    return TRUE;
}

static gboolean playercmd_volume(PlayerctlPlayer *player, gchar **argv, gint argc,
                                 gchar **output, GError **error) {
    const gchar *volume = argv[1];
    gdouble level;

    if (volume) {
        if (format_string_arg != NULL) {
            g_set_error(error, playerctl_cli_error_quark(), 1,
                        "format strings are not supported on command functions.");
            return FALSE;
        }
        char *endptr = NULL;
        size_t last = strlen(volume) - 1;

        if (volume[last] == '+' || volume[last] == '-') {
            gdouble adjustment = strtod(volume, &endptr);

            if (volume == endptr) {
                g_set_error(error, playerctl_cli_error_quark(), 1,
                            "Could not parse volume as a number: %s\n", volume);
                return FALSE;
            }

            if (volume[last] == '-') {
                adjustment *= -1;
            }

            g_object_get(player, "volume", &level, NULL);
            level += adjustment;
        } else {
            level = strtod(volume, &endptr);
            if (volume == endptr) {
                g_set_error(error, playerctl_cli_error_quark(), 1,
                            "Could not parse volume as a number: %s\n", volume);
                return FALSE;
            }
        }

        gboolean can_control = FALSE;
        g_object_get(player, "can-control", &can_control, NULL);

        if (!can_control) {
            return FALSE;
        }

        g_object_set(player, "volume", level, NULL);
    } else {
        g_object_get(player, "volume", &level, NULL);

        if (formatter != NULL) {
            GError *tmp_error = NULL;
            GVariantDict *context =
                playerctl_formatter_default_template_context(formatter, player, NULL);
            gchar *formatted =
                playerctl_formatter_expand_format(formatter, context, &tmp_error);
            if (tmp_error != NULL) {
                g_propagate_error(error, tmp_error);
                return FALSE;
            }
            *output = g_strdup_printf("%s\n", formatted);
            g_free(formatted);
        } else {
            *output = g_strdup_printf("%f\n", level);
        }
    }

    return TRUE;
}

static gboolean playercmd_status(PlayerctlPlayer *player, gchar **argv, gint argc,
                                 gchar **output, GError **error) {
    GError *tmp_error = NULL;

    if (formatter != NULL) {
        GVariantDict *context =
            playerctl_formatter_default_template_context(formatter, player, NULL);
        gchar *formatted =
            playerctl_formatter_expand_format(formatter, context, &tmp_error);
        if (tmp_error != NULL) {
            g_propagate_error(error, tmp_error);
            g_variant_dict_unref(context);
            return FALSE;
        }

        *output = g_strdup_printf("%s\n", formatted);

        g_variant_dict_unref(context);
        g_free(formatted);
    } else {
        PlayerctlPlaybackStatus status = 0;
        g_object_get(player, "playback-status", &status, NULL);
        const gchar *status_str = pctl_playback_status_to_string(status);
        assert(status_str != NULL);
        *output = g_strdup_printf("%s\n", status_str);
    }

    return TRUE;
}

static gboolean playercmd_shuffle(PlayerctlPlayer *player, gchar **argv, gint argc,
                                  gchar **output, GError **error) {
    GError *tmp_error = NULL;

    if (argc > 1) {
        gchar *status_str = argv[1];
        gboolean status = FALSE;

        if (strcasecmp(status_str, "on") == 0) {
            status = TRUE;
        } else if (strcasecmp(status_str, "off") == 0) {
            status = FALSE;
        } else {
            g_set_error(error, playerctl_cli_error_quark(), 1,
                        "Got unknown loop status: '%s' (expected 'none', "
                        "'playlist', or 'track').", argv[1]);
            return FALSE;
        }

        playerctl_player_set_shuffle(player, status, &tmp_error);
        if (tmp_error != NULL) {
            g_propagate_error(error, tmp_error);
            return FALSE;
        }
    } else {
        if (formatter != NULL) {
            GVariantDict *context =
                playerctl_formatter_default_template_context(formatter, player, NULL);
            gchar *formatted =
                playerctl_formatter_expand_format(formatter, context, &tmp_error);
            if (tmp_error != NULL) {
                g_propagate_error(error, tmp_error);
                g_variant_dict_unref(context);
                return FALSE;
            }

            *output = g_strdup_printf("%s\n", formatted);

            g_variant_dict_unref(context);
            g_free(formatted);
        } else {
            gboolean status = FALSE;
            g_object_get(player, "shuffle", &status, NULL);
            if (status) {
                *output = g_strdup("On\n");
            } else {
                *output = g_strdup("Off\n");
            }
        }
    }

    return TRUE;

}

static gboolean playercmd_loop(PlayerctlPlayer *player, gchar **argv, gint argc,
                               gchar **output, GError **error) {
    GError *tmp_error = NULL;

    if (argc > 1) {
        gchar *status_str = argv[1];
        PlayerctlLoopStatus status = 0;
        if (!pctl_parse_loop_status(status_str, &status)) {
            g_set_error(error, playerctl_cli_error_quark(), 1,
                        "Got unknown loop status: '%s' (expected 'none', "
                        "'playlist', or 'track').", argv[1]);
            return FALSE;
        }

        playerctl_player_set_loop_status(player, status, &tmp_error);
        if (tmp_error != NULL) {
            g_propagate_error(error, tmp_error);
            return FALSE;
        }
    } else {
        if (formatter != NULL) {
            GVariantDict *context =
                playerctl_formatter_default_template_context(formatter, player, NULL);
            gchar *formatted =
                playerctl_formatter_expand_format(formatter, context, &tmp_error);
            if (tmp_error != NULL) {
                g_propagate_error(error, tmp_error);
                g_variant_dict_unref(context);
                return FALSE;
            }

            *output = g_strdup_printf("%s\n", formatted);

            g_variant_dict_unref(context);
            g_free(formatted);
        } else {
            PlayerctlLoopStatus status = 0;
            g_object_get(player, "loop-status", &status, NULL);
            const gchar *status_str = pctl_loop_status_to_string(status);
            assert(status_str != NULL);
            *output = g_strdup_printf("%s\n", status_str);
        }
    }

    return TRUE;
}

static gboolean playercmd_metadata(PlayerctlPlayer *player, gchar **argv, gint argc,
                                   gchar **output, GError **error) {
    GError *tmp_error = NULL;

    gboolean can_play = FALSE;
    g_object_get(player, "can-play", &can_play, NULL);

    if (!can_play) {
        // skip if no current track
        return FALSE;
    }

    if (format_string_arg != NULL) {
        gchar *data = get_metadata_formatted(player, &tmp_error);
        if (tmp_error) {
            g_propagate_error(error, tmp_error);
            return FALSE;
        }
        if (data != NULL) {
            *output = g_strdup_printf("%s\n", data);
            g_free(data);
        } else {
            return FALSE;
        }
    } else if (argc == 1) {
        gchar *data = playerctl_player_print_metadata_prop(player, NULL, &tmp_error);
        if (tmp_error) {
            g_propagate_error(error, tmp_error);
            return FALSE;
        }

        if (data != NULL) {
            *output = g_strdup_printf("%s\n", data);
            g_free(data);
        } else {
            return FALSE;
        }
    } else {
        for (int i = 1; i < argc; ++i) {
            const gchar *type = argv[i];
            gchar *data;

            if (g_strcmp0(type, "artist") == 0) {
                data = playerctl_player_get_artist(player, &tmp_error);
            } else if (g_strcmp0(type, "title") == 0) {
                data = playerctl_player_get_title(player, &tmp_error);
            } else if (g_strcmp0(type, "album") == 0) {
                data = playerctl_player_get_album(player, &tmp_error);
            } else {
                data = playerctl_player_print_metadata_prop(player, type, &tmp_error);
            }

            if (tmp_error) {
                g_propagate_error(error, tmp_error);
                return FALSE;
            }

            if (data != NULL) {
                *output = g_strdup_printf("%s\n", data);
                g_free(data);
            } else {
                return FALSE;
            }
        }
    }

    return TRUE;
}

static void playercmd_follow_callback(PlayerctlPlayer *player, struct playercmd_args *args) {
    if (select_all_players) {
        GList *match = g_list_find(followed_players, player);
        if (match == NULL) {
            return;
        }
        followed_players = g_list_remove_link(followed_players, match);
        followed_players = g_list_prepend(followed_players, player);
    }

    GError *tmp_error = NULL;
    followed_players_execute_command(&tmp_error);
    if (tmp_error != NULL) {
        g_printerr("Error while executing command: %s\n", tmp_error->message);
        g_clear_error(&tmp_error);
        g_main_loop_quit(main_loop);
    }
}

static gboolean playercmd_tick_callback(gpointer data) {
    GError *tmp_error = NULL;
    followed_players_execute_command(&tmp_error);
    if (tmp_error != NULL) {
        g_printerr("Error while executing command: %s\n", tmp_error->message);
        g_clear_error(&tmp_error);
        g_main_loop_quit(main_loop);
        return FALSE;
    }
    return TRUE;
}

struct player_command {
    const gchar *name;
    gboolean (*func)(PlayerctlPlayer *player, gchar **argv, gint argc, gchar **output, GError **error);
    gboolean supports_format;
    const gchar *follow_signal;
} player_commands[] = {
    {"open", &playercmd_open, FALSE, NULL},
    {"play", &playercmd_play, FALSE, NULL},
    {"pause", &playercmd_pause, FALSE, NULL},
    {"play-pause", &playercmd_play_pause, FALSE, NULL},
    {"stop", &playercmd_stop, FALSE, NULL},
    {"next", &playercmd_next, FALSE, NULL},
    {"previous", &playercmd_previous, FALSE, NULL},
    {"position", &playercmd_position, TRUE, "seeked"},
    {"volume", &playercmd_volume, TRUE, "volume"},
    {"status", &playercmd_status, TRUE, "playback-status"},
    {"loop", &playercmd_loop, TRUE, "loop-status"},
    {"shuffle", &playercmd_shuffle, TRUE, "shuffle"},
    {"metadata", &playercmd_metadata, TRUE, "metadata"},
};

static const struct player_command *get_player_command(gchar **argv, gint argc, GError **error) {
    for (gsize i = 0; i < LENGTH(player_commands); ++i) {
        const struct player_command command = player_commands[i];
        if (g_strcmp0(command.name, argv[0]) == 0) {
            if (format_string_arg != NULL && !command.supports_format) {
                g_set_error(error, playerctl_cli_error_quark(), 1,
                            "format strings are not supported on command: %s", argv[0]);
                return NULL;
            }

            if (follow && (command.follow_signal == NULL)) {
                g_set_error(error, playerctl_cli_error_quark(), 1,
                            "follow is not supported on command: %s", argv[0]);
                return NULL;
            }

            return &player_commands[i];
        }
    }

    g_set_error(error, playerctl_cli_error_quark(), 1,
                "Command not recognized: %s", argv[0]);

    return NULL;
}

static const GOptionEntry entries[] = {
    {"player", 'p', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &player_arg,
     "A comma separated list of names of players to control (default: the "
     "first available player)",
     "NAME"},
    {"all-players", 'a', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
     &select_all_players, "Select all available players to be controlled",
     NULL},
    {"ignore-player", 'i', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &ignore_player_arg,
     "A comma separated list of names of players to ignore.", "IGNORE"},
    {"format", 'f', G_OPTION_FLAG_NONE, G_OPTION_ARG_STRING, &format_string_arg,
     "A format string for printing properties and metadata", NULL},
    {"follow", 'F', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE, &follow,
     "Block and append the query to output when it changes. Exit when the players exit.",
     NULL},
    {"list-all", 'l', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
     &list_all_players_and_exit,
     "List the names of running players that can be controlled", NULL},
    {"version", 'v', G_OPTION_FLAG_NONE, G_OPTION_ARG_NONE,
     &print_version_and_exit, "Print version information", NULL},
    {G_OPTION_REMAINING, 0, 0, G_OPTION_ARG_STRING_ARRAY, &command, NULL,
     "COMMAND"},
    {NULL}};

static gboolean parse_setup_options(int argc, char *argv[], GError **error) {
    static const gchar *description =
        "Available Commands:"
        "\n  play                    Command the player to play"
        "\n  pause                   Command the player to pause"
        "\n  play-pause              Command the player to toggle between "
        "play/pause"
        "\n  stop                    Command the player to stop"
        "\n  next                    Command the player to skip to the next track"
        "\n  previous                Command the player to skip to the previous "
        "track"
        "\n  position [OFFSET][+/-]  Command the player to go to the position or "
        "seek forward/backward OFFSET in seconds"
        "\n  volume [LEVEL][+/-]     Print or set the volume to LEVEL from 0.0 "
        "to 1.0"
        "\n  status                  Get the play status of the player"
        "\n  metadata [KEY...]       Print metadata information for the current "
        "track. If KEY is passed,"
        "\n                          print only those values. KEY may be artist,"
        "title, album, or any key found in the metadata."
        "\n  open [URI]              Command for the player to open given URI."
        "\n                          URI can be either file path or remote URL."
        "\n  loop [STATUS]           Print or set the loop status."
        "\n                          Can be \"None\", \"Track\", or \"Playlist\"."
        "\n  shuffle [STATUS]        Print or set the shuffle status."
        "\n                          Can be \"On\" or \"Off\".";

    static const gchar *summary =
        "  For players supporting the MPRIS D-Bus specification";
    GOptionContext *context = NULL;
    gboolean success;

    context = g_option_context_new("- Controller for media players");
    g_option_context_add_main_entries(context, entries, NULL);
    g_option_context_set_description(context, description);
    g_option_context_set_summary(context, summary);

    success = g_option_context_parse(context, &argc, &argv, error);

    if (!success) {
        g_option_context_free(context);
        return FALSE;
    }

    if (command == NULL && !print_version_and_exit &&
        !list_all_players_and_exit) {
        gchar *help = g_option_context_get_help(context, TRUE, NULL);
        g_set_error(error, playerctl_cli_error_quark(), 1,
                    "No command entered\n\n%s", help);
        g_option_context_free(context);
        g_free(help);
        return FALSE;
    }

    g_option_context_free(context);
    return TRUE;
}

static GList *parse_player_list(gchar *player_list_arg) {
    GList *players = NULL;
    if (player_list_arg == NULL) {
        return NULL;
    }

    const gchar *delim = ",";
    gchar *token = strtok(player_list_arg, delim);
    while (token != NULL) {
        players = g_list_append(players, g_strdup(g_strstrip(token)));
        token = strtok(NULL, ",");
    }

    return players;
}

static int handle_version_flag() {
    g_print("v%s\n", PLAYERCTL_VERSION_S);
    return 0;
}

static int handle_list_all_flag() {
    GError *tmp_error = NULL;
    GList *player_names = playerctl_list_players(&tmp_error);

    if (tmp_error != NULL) {
        g_printerr("%s\n", tmp_error->message);
        return 1;
    }

    if (player_names == NULL) {
        g_printerr("No players were found\n");
        return 0;
    }

    GList *next = player_names;
    while (next != NULL) {
        gchar *name = next->data;
        printf("%s\n", name);
        next = next->next;
    }

    g_list_free_full(player_names, g_free);
    return 0;
}

static GList *select_players(GList *players, GList *all_players, GList *ignored_players) {
    GList *result = NULL;

    if (players == NULL) {
        // select the players that are not ignored
        GList *all_players_next = all_players;
        while (all_players_next != NULL) {
            gchar *current_name = all_players_next->data;
            gboolean ignored =
                (g_list_find_custom(ignored_players, current_name,
                                    (GCompareFunc)pctl_player_name_instance_compare) != NULL);

            if (!ignored && !g_list_find(result, current_name)) {
                result = g_list_append(result, current_name);
            }

            all_players_next = all_players_next->next;
        }

        return result;
    }

    GList *players_next = players;
    while (players_next) {
        gchar *player_name = players_next->data;

        GList *all_players_next = all_players;
        while (all_players_next != NULL) {
            gchar *current_name = all_players_next->data;

            if (pctl_player_name_instance_compare(player_name, current_name) == 0) {
                gboolean ignored =
                    (g_list_find_custom(ignored_players, current_name,
                                         (GCompareFunc)pctl_player_name_instance_compare) != NULL);
                if (!ignored && !g_list_find(result, current_name)) {
                    result = g_list_append(result, current_name);
                }
            }

            all_players_next = all_players_next->next;
        }

        players_next = players_next->next;
    }

    return result;
}

static gchar *player_id_from_bus_name(const gchar *bus_name) {
    const size_t prefix_len = strlen(MPRIS_PREFIX);

    if (bus_name == NULL ||
            !g_str_has_prefix(bus_name, MPRIS_PREFIX) ||
            strlen(bus_name) <= prefix_len) {
        return NULL;
    }

    return g_strdup(bus_name + prefix_len);
}

static void add_followed_player(PlayerctlPlayer *player, GError **error) {
    GError *tmp_error = NULL;

    if (player == NULL) {
        return;
    }
    const struct player_command *player_cmd =
        get_player_command(playercmd_args->argv, playercmd_args->argc,
                           &tmp_error);
    if (tmp_error != NULL) {
        g_propagate_error(error, tmp_error);
        return;
    }

    assert(player_cmd->follow_signal != NULL);
    g_signal_connect(G_OBJECT(player), player_cmd->follow_signal,
                     G_CALLBACK(playercmd_follow_callback),
                     playercmd_args);

    if (formatter != NULL) {
        for (gsize i = 0; i < LENGTH(player_commands); ++i) {
            const struct player_command cmd = player_commands[i];
            if (&cmd != player_cmd &&
                    cmd.follow_signal != NULL &&
                    g_strcmp0(cmd.name, "metadata") != 0 &&
                    playerctl_formatter_contains_key(formatter, cmd.name)) {
                g_signal_connect(G_OBJECT(player), cmd.follow_signal,
                                 G_CALLBACK(playercmd_follow_callback),
                                 playercmd_args);
            }
        }
    }

    followed_players = g_list_prepend(followed_players, player);
}

static void add_followed_player_by_name(gchar *player_name, GError **error) {
    // check and see if it's in the list
    GError *tmp_error = NULL;
    PlayerctlPlayer *player = NULL;

    if (player_name == NULL) {
        return;
    }

    GList *next = followed_players;
    while (next != NULL) {
        player = PLAYERCTL_PLAYER(next->data);
        gchar *player_id = NULL;
        g_object_get(player, "player-id", &player_id, NULL);
        if (g_strcmp0(player_id, player_name) == 0) {
            g_free(player_id);
            return;
        }

        g_free(player_id);
        next = next->next;
    }

    player = playerctl_player_new(player_name, &tmp_error);
    if (tmp_error != NULL) {
        g_propagate_error(error, tmp_error);
        return;
    }

    add_followed_player(player, &tmp_error);
    if (tmp_error != NULL) {
        g_propagate_error(error, tmp_error);
        return;
    }
}

static void remove_followed_player_by_name(gchar *player_name) {
    GList *next = followed_players;
    while (next != NULL) {
        PlayerctlPlayer *player = PLAYERCTL_PLAYER(next->data);
        gchar *player_id = NULL;
        g_object_get(player, "player-id", &player_id, NULL);
        if (g_strcmp0(player_id, player_name) == 0) {
            followed_players = g_list_remove_link(followed_players, next);
            g_list_free_full(next, g_object_unref);
            g_free(player_id);
            break;
        }
        g_free(player_id);
        next = next->next;
    }
}

static void clear_followed_players() {
    g_list_free_full(followed_players, g_object_unref);
    followed_players = NULL;
}

static void followed_players_execute_command(GError **error) {
    GError *tmp_error = NULL;

    const struct player_command *player_cmd =
        get_player_command(playercmd_args->argv, playercmd_args->argc,
                           &tmp_error);
    if (tmp_error != NULL) {
        g_propagate_error(error, tmp_error);
        return;
    }
    assert(player_cmd->func != NULL);

    gboolean did_command = FALSE;
    GList *next = followed_players;
    while (next != NULL) {
        PlayerctlPlayer *player = PLAYERCTL_PLAYER(next->data);
        gchar *output = NULL;

        gboolean result =
            player_cmd->func(player, playercmd_args->argv, playercmd_args->argc,
                             &output, &tmp_error);
        if (tmp_error != NULL) {
            g_propagate_error(error, tmp_error);
            g_free(output);
            return;
        }

        if (output != NULL) {
            cli_print_output(output);
        }
        did_command = did_command || result;

        if (result || !select_all_players) {
            break;
        }

        next = next->next;
    }

    if (!did_command) {
        cli_print_output(NULL);
    }
}

struct owner_changed_user_data {
    GList *all_players;
    GList *players;
    GList *ignored_players;
};

static void dbus_name_owner_changed_callback(GDBusProxy *proxy, gchar *sender_name,
                                             gchar *signal_name, GVariant *parameters,
                                             gpointer _data) {
    struct owner_changed_user_data *data = _data;
    GList *selected_players = NULL;
    GError *error = NULL;

    if (g_strcmp0(signal_name, "NameOwnerChanged") != 0) {
        return;
    }

    if (!g_variant_is_of_type(parameters, G_VARIANT_TYPE("(sss)"))) {
        g_warning("Got unknown parameters on org.freedesktop.DBus "
                  "NameOwnerChange signal: %s",
                  g_variant_get_type_string(parameters));
        return;
    }

    GVariant *name_variant = g_variant_get_child_value(parameters, 0);
    const gchar *name = g_variant_get_string(name_variant, NULL);

    gchar *player_id = player_id_from_bus_name(name);

    if (player_id == NULL) {
        g_variant_unref(name_variant);
        return;
    }

    GVariant *previous_owner_variant = g_variant_get_child_value(parameters, 1);
    const gchar *previous_owner = g_variant_get_string(previous_owner_variant, NULL);

    GVariant *new_owner_variant = g_variant_get_child_value(parameters, 2);
    const gchar *new_owner = g_variant_get_string(new_owner_variant, NULL);

    // update the list of all players
    GList *player_entry = NULL;
    if (strlen(new_owner) == 0 && strlen(previous_owner) != 0) {
        // the name has vanished
        player_entry =
            g_list_find_custom(data->all_players, name + strlen(MPRIS_PREFIX), (GCompareFunc)g_strcmp0);

        if (player_entry != NULL) {
            data->all_players = g_list_remove_link(data->all_players, player_entry);
            g_list_free_full(player_entry, g_free);
        }

        remove_followed_player_by_name(player_id);
    } else if (strlen(previous_owner) == 0 && strlen(new_owner) != 0) {
        // the name has appeared
        player_entry =
            g_list_find_custom(data->all_players, name + strlen(MPRIS_PREFIX), (GCompareFunc)g_strcmp0);
        if (player_entry == NULL) {
            data->all_players =
                g_list_prepend(data->all_players, g_strdup(name + strlen(MPRIS_PREFIX)));
        }
    }

    // update the followed players
    selected_players = select_players(data->players, data->all_players, data->ignored_players);
    if (selected_players != NULL) {
        // there is a new candidate for following
        gchar *first_selected = selected_players->data;
        guint followed_len = g_list_length(followed_players);

        if (followed_len == 0) {
            // not following a player, follow this one
            add_followed_player_by_name(first_selected, &error);
            if (error != NULL) {
                goto out;
            }
        } else {
            if (data->players == NULL) {
                // if no player arguments were passed, always follow the most
                // recently opened player.
                if (!select_all_players) {
                    clear_followed_players();
                }
                add_followed_player_by_name(first_selected, &error);
                if (error != NULL) {
                    goto out;
                }
            } else {
                // if player arguments were passed, follow the most recently
                // opened player in the order they were passed on the command
                // line.
                GList *next = data->players;
                while (next != NULL) {
                    gchar *name = next->data;

                    GList *match =
                        g_list_find_custom(selected_players, name,
                                           (GCompareFunc)pctl_player_name_instance_compare);

                    if (match != NULL) {
                        gchar *match_name = match->data;
                        if (!select_all_players) {
                            clear_followed_players();
                        }
                        add_followed_player_by_name(match_name, &error);
                        if (error != NULL) {
                            goto out;
                        }
                        break;
                    }

                    next = next->next;
                }
            }
        }
    }

    // rerun the command on the updated list of followed players
    followed_players_execute_command(&error);

out:
    if (error != NULL) {
        g_printerr("Could not connect to player: %s\n", error->message);
        g_clear_error(&error);
        g_main_loop_quit(main_loop);
    }
    g_list_free(selected_players);
    g_variant_unref(name_variant);
    g_variant_unref(previous_owner_variant);
    g_variant_unref(new_owner_variant);
}

int main(int argc, char *argv[]) {
    PlayerctlPlayer *player;
    GError *error = NULL;
    guint num_commands = 0;
    int status = 0;

    // seems to be required to print unicode (see #8)
    setlocale(LC_CTYPE, "");

    if (!parse_setup_options(argc, argv, &error)) {
        g_printerr("%s\n", error->message);
        g_clear_error(&error);
        exit(0);
    }

    if (print_version_and_exit) {
        int result = handle_version_flag();
        exit(result);
    }

    if (list_all_players_and_exit) {
        int result = handle_list_all_flag();
        exit(result);
    }

    num_commands = g_strv_length(command);

    GList *all_players = playerctl_list_players(&error);
    if (error != NULL) {
        g_printerr("%s\n", error->message);
        g_clear_error(&error);
        exit(1);
    }

    if (format_string_arg != NULL) {
        formatter = playerctl_formatter_new(format_string_arg, &error);
        if (error != NULL) {
            g_printerr("Could not execute command: %s\n", error->message);
            g_clear_error(&error);
            exit(1);
        }
    }

    const struct player_command *player_cmd = get_player_command(command, num_commands, &error);
    if (error != NULL) {
        g_printerr("Could not execute command: %s\n", error->message);
        g_clear_error(&error);
        exit(1);
    }

    if (all_players == NULL && !follow) {
        g_printerr("No players were found\n");
        exit(0);
    }

    GList *players = parse_player_list(player_arg);
    GList *ignored_players = parse_player_list(ignore_player_arg);
    GList *selected_players = select_players(players, all_players, ignored_players);

    if (selected_players == NULL && !follow) {
        g_printerr("No players were found\n");
        goto end;
    }

    playercmd_args = playercmd_args_create(command, num_commands);
    GList *next = selected_players;
    while (next != NULL) {
        gchar *player_name = next->data;

        if (follow) {
            add_followed_player_by_name(player_name, &error);
            if (error != NULL) {
                g_printerr("Connection to player failed: %s\n", error->message);
                status = 1;
                goto end;
            }

            if (select_all_players) {
                next = next->next;
                continue;
            }

            break;
        }

        player = playerctl_player_new(player_name, &error);
        if (error != NULL) {
            g_printerr("Connection to player failed: %s\n", error->message);
            status = 1;
            goto end;
        }

        gchar *output = NULL;
        gboolean result = player_cmd->func(player, command, num_commands, &output, &error);
        if (error != NULL) {
            g_printerr("Could not execute command: %s\n", error->message);
            g_clear_error(&error);
            g_object_unref(player);
            g_free(output);
            status = 1;
            break;
        }

        if (output != NULL) {
            printf("%s", output);
            g_free(output);
        }

        g_object_unref(player);

        if (result && !select_all_players) {
            break;
        }

        next = next->next;
    }

end:
    g_list_free(selected_players);

    if (status == 0 && follow) {
        followed_players_execute_command(&error);
        if (error != NULL) {
            g_printerr("Connection to player failed: %s\n", error->message);
            status = 1;
            goto end;
        }

        struct owner_changed_user_data *data =
            calloc(1, sizeof(struct owner_changed_user_data));
        data->players = players;
        data->all_players = all_players;
        data->ignored_players = ignored_players;

        GDBusProxy *proxy =
            g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION,
                                          G_DBUS_PROXY_FLAGS_NONE, NULL,
                                          "org.freedesktop.DBus",
                                          "/org/freedesktop/DBus",
                                          "org.freedesktop.DBus", NULL,
                                          &error);
        if (error != NULL) {
            g_printerr("%s\n", error->message);
            g_clear_error(&error);
            status = 1;
            goto proxy_err_out;
        }

        g_signal_connect(G_DBUS_PROXY(proxy), "g-signal",
                         G_CALLBACK(dbus_name_owner_changed_callback),
                         data);

        if (formatter != NULL &&
                playerctl_formatter_contains_key(formatter, "position")) {
            g_timeout_add(1000, playercmd_tick_callback, NULL);
        }

        main_loop = g_main_loop_new(NULL, FALSE);
        g_main_loop_run(main_loop);
        g_main_loop_unref(main_loop);

proxy_err_out:
        free(data);
        playercmd_args_destroy(playercmd_args);
    }

    playerctl_formatter_destroy(formatter);
    g_free(last_output);
    clear_followed_players();
    g_list_free_full(all_players, g_free);
    g_list_free_full(players, g_free);
    g_list_free_full(ignored_players, g_free);

    exit(status);
}
