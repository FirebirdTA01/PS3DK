/*! \file cell/sysutil_game_exec.h
 \brief Boot information and exit parameters for a game launched from
        another title (libsysutil_game_exec_stub).

  cellGameGetBootGameInfo reports how the title was started (its game
  type), the directory it was started from and the execdata word the
  launching title passed; cellGameSetExitParam sets the word handed back
  on exit.  The three path queries fill CELL_GAME_PATH_MAX-byte buffers
  with the HOME launch-option and data export / import paths.  The
  CELL_GAME_* return codes and sizes are those of <cell/sysutil_gamecontent.h>.
*/

#ifndef PS3TC_CELL_SYSUTIL_GAME_EXEC_H
#define PS3TC_CELL_SYSUTIL_GAME_EXEC_H

#include <sysutil/sysutil_common.h>
#include <cell/sysutil_gamecontent.h>   /* CELL_GAME_* codes, sizes, game types */

#ifdef __cplusplus
extern "C" {
#endif

int cellGameGetBootGameInfo(unsigned int *type, char *dirName, unsigned int *execdata);
int cellGameSetExitParam(unsigned int execdata);
int cellGameGetHomeLaunchOptionPath(char *commonPath, char *personalPath);
int cellGameGetHomeDataExportPath(char *exportPath);
int cellGameGetHomeDataImportPath(char *importPath);

#ifdef __cplusplus
}
#endif

#endif /* PS3TC_CELL_SYSUTIL_GAME_EXEC_H */
