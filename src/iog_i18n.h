#ifndef IOG_I18N_H
#define IOG_I18N_H

/* Runtime localization (language packs, see src/iog_i18n.c). */
void IogI18nInit(void);
void IogI18nFrame(void);
int IogI18nSetLanguage(const char *code);   /* "original" restores the ROM */
const char *IogI18nLanguage(void);
/* Select the next installed language (applied once no message is on screen)
 * and remember it in language.txt. Returns its display name. */
const char *IogI18nCycle(void);
int IogI18nAvailable(const char *code);

#endif
