// Hardware description for the srally2 ROM set (Model 3 step 2.0 board). Hardware facts only, no ROM content. GPLv3.
#include "Game.h"
Game MakeSrally2()
{
	Game g;
	g.name = "srally2"; g.title = "srally2"; g.version = "Export"; g.manufacturer = "-"; g.year = 1998;
	g.stepping = "2.0"; g.mpeg_board = "DSB2";
	g.audio = Game::QUAD_1_LR_2_FR_MIX;
	g.netboard_present = true;
	g.inputs = Game::INPUT_COMMON | Game::INPUT_VEHICLE | Game::INPUT_SHIFT4 | Game::INPUT_HANDBRAKE | Game::INPUT_VIEWCHANGE;
	g.driveboard_type = Game::DRIVE_BOARD_WHEEL;
	return g;
}
