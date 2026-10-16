/* tinyfiledialogs for platforms with no native dialogs (Nintendo Switch):
 * every dialog is "cancelled"; messages go to stderr. Files are picked with
 * the launcher's own in-app browser there. */
#include <stdio.h>

int tinyfd_messageBox(char const *title, char const *message, char const *dialog_type,
                      char const *icon_type, int default_button)
{
    (void)dialog_type; (void)icon_type;
    fprintf(stderr, "[%s] %s\n", title ? title : "", message ? message : "");
    return default_button;
}

char *tinyfd_openFileDialog(char const *title, char const *default_path, int n,
                            char const *const *patterns, char const *desc, int multi)
{
    (void)title; (void)default_path; (void)n; (void)patterns; (void)desc; (void)multi;
    return NULL;
}

char *tinyfd_selectFolderDialog(char const *title, char const *default_path)
{
    (void)title; (void)default_path;
    return NULL;
}

char *tinyfd_saveFileDialog(char const *title, char const *default_path, int n,
                            char const *const *patterns, char const *desc)
{
    (void)title; (void)default_path; (void)n; (void)patterns; (void)desc;
    return NULL;
}
