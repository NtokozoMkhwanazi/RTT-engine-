#ifndef EDITOR_PHOSPHOR_ICONS_H
#define EDITOR_PHOSPHOR_ICONS_H

#include <unordered_map>
#include <string>

namespace PhosphorIcons {
    enum Icon {
        None = 0,

        // Media controls
        Play,
        Pause,
        Stop,
        PlayPause,
        SkipBack,
        SkipForward,
        Rewind,
        FastForward,

        // File operations
        Folder,
        FolderOpen,
        FolderPlus,
        File,
        FilePlus,
        FileText,
        Save,
        Download,
        Upload,

        // Editing
        Pencil,
        PencilSimple,
        Eraser,
        Copy,
        Scissors,
        Clock,
        Plus,
        Minus,
        X,

        // View
        Eye,
        EyeSlash,
        MagnifyingGlass,
        ZoomIn,
        ZoomOut,

        // 3D Editor
        Cube,
        CubeTransparent,
        Sphere,
        Triangle,
        GridFour,
        ArrowsOut,
        ArrowsIn,
        ArrowCounterClockwise,

        // UI Layout
        SquaresFour,
        Rows,
        Columns,
        List,
        CaretDown,
        CaretRight,
        CaretLeft,
        CaretUp,

        // Navigation
        House,
        SignOut,
        ArrowLeft,
        ArrowRight,
        ArrowUp,
        ArrowDown,

        // Entity types (outliner)
        Camera,
        Lightbulb,
        MapPin,

        // Status
        WarningCircle,
        CheckCircle,
        Info,
        Bug,
        Gear,
        GearSix,

        // Misc
        Star,
        Heart,
        User,
        Users,
        Chat,
        Bell,
        Trash,
        DotsThree
    };

    static const std::unordered_map<Icon, const char*> iconCodepoints = {
        {Play, "\ue9c4"},
        {Pause, "\uea38"},
        {Stop, "\uea3a"},
        {PlayPause, "\ue9c6"},
        {SkipBack, "\uea30"},
        {SkipForward, "\uea32"},
        {Rewind, "\uea7a"},
        {FastForward, "\uea78"},

        {Folder, "\ue24a"},
        {FolderOpen, "\ue256"},
        {FolderPlus, "\ue258"},
        {File, "\ue230"},
        {FilePlus, "\ue236"},
        {FileText, "\ue23a"},
        {Save, "\ue248"},
        {Download, "\ue20a"},
        {Upload, "\ue9be"},

        {Pencil, "\ue270"},
        {PencilSimple, "\ue272"},
        {Eraser, "\ue21e"},
        {Copy, "\ue1ca"},
        {Scissors, "\ueaa2"},
        {Clock, "\ue19a"},
        {Plus, "\uee61"},
        {Minus, "\uee63"},
        {X, "\ue9c8"},

        {Eye, "\ue220"},
        {EyeSlash, "\ue224"},
        {MagnifyingGlass, "\ue268"},
        {ZoomIn, "\uef76"},
        {ZoomOut, "\uef74"},

        {Cube, "\ue1da"},
        {CubeTransparent, "\uec7c"},
        {Sphere, "\ueabc"},
        {Triangle, "\ue61e"},
        {GridFour, "\ue898"},
        {ArrowsOut, "\ue0a2"},
        {ArrowsIn, "\ue09a"},
        {ArrowCounterClockwise, "\ue038"},

        {SquaresFour, "\ue190"},
        {Rows, "\ue5e4"},
        {Columns, "\ue546"},
        {List, "\ue8b4"},
        {CaretDown, "\ue136"},
        {CaretRight, "\ue13a"},
        {CaretLeft, "\ue138"},
        {CaretUp, "\ue13c"},

        {House, "\ue3a8"},
        {SignOut, "\xea\xac"},
        {ArrowLeft, "\ue058"},
        {ArrowRight, "\ue06c"},
        {ArrowUp, "\ue08e"},
        {ArrowDown, "\ue03e"},

        {Camera, "\ue10e"},
        {Lightbulb, "\ue2dc"},
        {MapPin, "\ue316"},

        {WarningCircle, "\uea48"},
        {CheckCircle, "\ue184"},
        {Info, "\ue9e4"},
        {Bug, "\ue5f4"},
        {Gear, "\ue988"},
        {GearSix, "\ue98c"},

        {Star, "\ue807"},
        {Heart, "\ue9d4"},
        {User, "\ue9d8"},
        {Users, "\ue9dc"},
        {Chat, "\ue15c"},
        {Bell, "\ue0ce"},
        {Trash, "\ueb48"},
        {DotsThree, "\ue1fe"}
    };

    inline const char* GetCodepoint(Icon icon) {
        auto it = iconCodepoints.find(icon);
        return it != iconCodepoints.end() ? it->second : nullptr;
    }

    inline bool HasIcon(Icon icon) {
        return iconCodepoints.find(icon) != iconCodepoints.end();
    }
}

#endif