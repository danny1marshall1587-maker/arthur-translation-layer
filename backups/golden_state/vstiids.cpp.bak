#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>
#include <pluginterfaces/vst/ivstaudioprocessor.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

namespace Steinberg {
    const FUID FUnknown::iid (0x00000000, 0x00000000, 0xC0000000, 0x00000046);
    const FUID IPluginFactory::iid (0x7A4D811C, 0x52114A1F, 0xAED9D2EE, 0x0B43BF9F);
    const FUID IPluginFactory2::iid (0x0007B650, 0xF24B4C0B, 0xA464EDB9, 0xF00B2ABB);
    namespace Vst {
        const FUID IComponent::iid (0xE831FF31, 0xF2D54301, 0x928EBBEE, 0x25697802);
        const FUID IAudioProcessor::iid (0x42043F99, 0xB7DA453C, 0xA569E79D, 0x9AAEC33D);
        const FUID IEditController::iid (0xDCD7513F, 0x8E374A93, 0x8055147D, 0x5053D501);
    }

    FUID::FUID (uint32 l1, uint32 l2, uint32 l3, uint32 l4) {
        data[0] = (uint8)((l1 & 0xFF000000) >> 24);
        data[1] = (uint8)((l1 & 0x00FF0000) >> 16);
        data[2] = (uint8)((l1 & 0x0000FF00) >> 8);
        data[3] = (uint8)((l1 & 0x000000FF));
        data[4] = (uint8)((l2 & 0xFF000000) >> 24);
        data[5] = (uint8)((l2 & 0x00FF0000) >> 16);
        data[6] = (uint8)((l2 & 0x0000FF00) >> 8);
        data[7] = (uint8)((l2 & 0x000000FF));
        data[8] = (uint8)((l3 & 0xFF000000) >> 24);
        data[9] = (uint8)((l3 & 0x00FF0000) >> 16);
        data[10] = (uint8)((l3 & 0x0000FF00) >> 8);
        data[11] = (uint8)((l3 & 0x000000FF));
        data[12] = (uint8)((l4 & 0xFF000000) >> 24);
        data[13] = (uint8)((l4 & 0x00FF0000) >> 16);
        data[14] = (uint8)((l4 & 0x0000FF00) >> 8);
        data[15] = (uint8)((l4 & 0x000000FF));
    }
}
