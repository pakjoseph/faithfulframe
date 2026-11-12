#include "faithfulframe-crop-selector.hpp"

int main()
{
	if (faithfulframe_select_crop_input(false) != faithfulframe_crop_input::legacy_aggregate)
		return 1;
	if (faithfulframe_select_crop_input(true) != faithfulframe_crop_input::safe_session)
		return 2;
	return 0;
}
