#pragma once

#define IDI_MAIN                    128
#define IDR_MAINMENU                130

// Dialogs (100% aligned with original RK Launcher .lang IDs)
#define IDD_ABOUT                   100
#define IDD_IMAGE_LOAD              101
#define IDD_LANGUAGE                103
#define IDD_PAGE_APPEARANCE         131
#define IDD_PAGE_POSITION           132
#define IDD_PAGE_BEHAVIOR           133
#define IDD_PAGE_LABEL              135
#define IDD_ITEM_PROPERTIES         136
#define IDD_ADD_DOCKLET             139
#define IDD_PAGE_EXCLUSIONS         140
#define IDD_PAGE_PERFORMANCE        141
#define IDD_PAGE_MENU               147

// String Table IDs (compatible with RK Launcher .lang files)
#define IDS_BEH_BOUNCE              105
#define IDS_SETTINGS_TITLE          106
#define IDS_BEH_ALWAYSONTOP         112
#define IDS_BEH_AUTOHIDE            113
#define IDS_BEH_LOCKED              114
#define IDS_BEH_MINIMIZETODOCK      115
#define IDS_BEH_SAVEONEXIT          116
#define IDS_BEH_SAVEONCHANGE        117
#define IDS_BEH_SAVEITEMS           118
#define IDS_BEH_NEWAPPS             119
#define IDS_BEH_HOTSPOT             120
#define IDS_BEH_HIDETASKBAR         121
#define IDS_BEH_NEVERTAKEFOCUS      122
#define IDS_BEH_STARTWITHWIN        123
#define IDS_BEH_CURRENTUSER         124
#define IDS_BEH_ALLUSERS            125
#define IDS_BEH_SHOWMINICON         126
#define IDS_EXC_NEWAPPS             130
#define IDS_EXC_MINIMIZE            131
#define IDS_LBL_NONE                140
#define IDS_LBL_COLOR_DEFAULT       141
#define IDS_LBL_COLOR_MORE          142
#define IDS_LANG_SELECT             170
#define IDS_LANG_INFO               171
#define IDS_LBL_PREVIEW_TEXT        190

// Menu Commands (exact match 0x8003..0x8030, 0x9c43)
#define IDM_DOCK_QUIT               0x8003
#define IDM_DOCK_ADD_SEPARATOR      0x8004
#define IDM_DOCK_ADD_DOCKLET        0x8005
#define IDM_DOCK_SETTINGS           0x8006
#define IDM_DOCK_LANGUAGE           0x8007
#define IDM_DOCK_MAGNIFY            0x8008
#define IDM_DOCK_ALWAYSONTOP        0x8009
#define IDM_DOCK_LOCKITEMS          0x800b
#define IDM_POS_BOTTOM_LEFT         0x800d
#define IDM_POS_BOTTOM_CENTER       0x800e
#define IDM_POS_BOTTOM_RIGHT        0x800f
#define IDM_POS_LEFT_TOP            0x8010
#define IDM_POS_LEFT_CENTER         0x8011
#define IDM_POS_LEFT_BOTTOM         0x8012
#define IDM_POS_RIGHT_TOP           0x8013
#define IDM_POS_RIGHT_CENTER        0x8014
#define IDM_POS_RIGHT_BOTTOM        0x8015
#define IDM_POS_TOP_LEFT            0x8016
#define IDM_POS_TOP_CENTER          0x8017
#define IDM_POS_TOP_RIGHT           0x8018
#define IDM_ITEM_PROPERTIES         0x8019
#define IDM_ITEM_DELETE_DOCKLET     0x801a
#define IDM_ITEM_REMOVE             0x801b
#define IDM_DOCK_DEL_SEPARATOR      0x801c
#define IDM_ITEM_CLOSE_WINDOW       0x801d
#define IDM_DOCK_AUTOHIDE           0x8021
#define IDM_DOCK_MINIMIZE_TO_DOCK   0x8022
#define IDM_DOCK_ABOUT              0x8030
#define IDM_DOCK_ADD_ITEM           0x9c43

// About Dialog controls (IDD 100)
#define IDC_ABOUT_VERSION           1064
#define IDC_ABOUT_CREDITS           1065
#define IDC_ABOUT_HOMEPAGE          1128

// Image Load controls (IDD 101)
#define IDC_IMG_TREE                1000
#define IDC_IMG_LIST                1001
#define IDC_IMG_PATH                1053
#define IDC_IMG_PREVIEW             1055
#define IDC_IMG_BROWSE              1070

// Language Dialog controls (IDD 103)
#define IDC_LANG_LIST               1002
#define IDC_LANG_DESC               1003

// Item Properties controls (IDD 136)
#define IDC_PROP_SAME_INSTANCE      1024
#define IDC_PROP_CUSTOM_ICON        1025
#define IDC_PROP_SHOW               1028
#define IDC_PROP_NAME               1046
#define IDC_PROP_LINK               1047
#define IDC_PROP_ARGS               1048
#define IDC_PROP_WORKDIR            1049
#define IDC_PROP_CHANGE_ICON        1050
#define IDC_PROP_ICON_GROUP         1052
#define IDC_PROP_BROWSE_LINK        1070

// Add Docklet controls (IDD 139)
#define IDC_DOCKLET_TYPE            1028
#define IDC_DOCKLET_PATH            1046
#define IDC_DOCKLET_LIST            1056
#define IDC_DOCKLET_AUTHOR          1057
#define IDC_DOCKLET_VERSION         1058
#define IDC_DOCKLET_NOTES           1059
#define IDC_DOCKLET_ADDED           1060
#define IDC_DOCKLET_ADD_BTN         1061

// Page Appearance controls (IDD 131)
#define IDC_APP_MAG_SLIDER          1001
#define IDC_APP_MAG_CHECK           1003
#define IDC_APP_THEME_COMBO         1004
#define IDC_APP_ALPHA_SLIDER        1005
#define IDC_APP_SMALL_SLIDER        1022
#define IDC_APP_MAG_VAL             1023
#define IDC_APP_SMALL_VAL           1024
#define IDC_APP_HIDE_IND            1025
#define IDC_APP_SPAN_EDIT           1027
#define IDC_APP_HIDE_POOF           1087
#define IDC_APP_DIST_SLIDER         1072
#define IDC_APP_DIST_VAL            1073

// Page Position controls (IDD 132 - .lang uses 1000, 1006..1018; 1105 in .lang is the center screen preview box)
#define IDC_POS_MARGIN_SLIDER       1000
#define IDC_POS_RAD_TL              1006
#define IDC_POS_RAD_TC              1007
#define IDC_POS_RAD_TR              1008
#define IDC_POS_RAD_RT              1009
#define IDC_POS_RAD_RC              1010
#define IDC_POS_RAD_RB              1011
#define IDC_POS_RAD_BR              1012
#define IDC_POS_RAD_BC              1013
#define IDC_POS_RAD_BL              1014
#define IDC_POS_RAD_LT              1015
#define IDC_POS_RAD_LC              1016
#define IDC_POS_RAD_LB              1017
#define IDC_POS_MULTIMON            1018
#define IDC_POS_MARGIN_VAL          2105

// Page Behavior controls (IDD 133)
#define IDC_BEH_ANIM_COUNT          1030
#define IDC_BEH_ANIM_SPIN           1031
#define IDC_BEH_ANIMATION           1039
#define IDC_BEH_MINPOS_COMBO        1040
#define IDC_BEH_ANIM_COMBO          1040
#define IDC_BEH_USE_EFFECT          1041
#define IDC_BEH_EFFECT_COMBO        1042
#define IDC_BEH_ANIM_NEW            1088
#define IDC_BEH_LIST                1090
#define IDC_BEH_HOTSPOT_DELAY       1095
#define IDC_BEH_HOTSPOT_SPIN        1096
#define IDC_BEH_ANIM_COUNT_LBL      1097
#define IDC_BEH_HOTSPOT_LBL         1098
#define IDC_BEH_HOTSPOT_MS          1099
#define IDC_BEH_SWW_REG             1100
#define IDC_BEH_SWW_STARTUP         1101
#define IDC_BEH_AUTOHIDE_LBL        1102
#define IDC_BEH_AUTOHIDE_DELAY      1103
#define IDC_BEH_SWW_USER            1104
#define IDC_BEH_AUTOHIDE_SPIN       1105
#define IDC_BEH_SWW_USER_LBL        1106
#define IDC_BEH_AUTOHIDE_MS         1107
#define IDC_BEH_HOTSPOT_FULL        1108

// Page Label controls (IDD 135)
#define IDC_LBL_BOLD                1024
#define IDC_LBL_ITALIC              1025
#define IDC_LBL_FONT                1028
#define IDC_LBL_SIZE                1029
#define IDC_LBL_COLOR_TEXT          1036
#define IDC_LBL_COLOR_EDGE          1037
#define IDC_LBL_COLOR_SHADOW        1038
#define IDC_LBL_PREVIEW             1045
#define IDC_LBL_ALPHA_TEXT          1066
#define IDC_LBL_ALPHA_EDGE          1067
#define IDC_LBL_ALPHA_SHADOW        1068
#define IDC_LBL_HIDE                1074
#define IDC_LBL_FADE                1085
#define IDC_LBL_ORDER               1089
#define IDC_LBL_PREVIEW_CANVAS      2045

// Page Exclusions controls (IDD 140)
#define IDC_EXC_TYPE_COMBO          1028
#define IDC_EXC_EDIT                1063
#define IDC_EXC_LIST                1079
#define IDC_EXC_DEL                 1081
#define IDC_EXC_ADD                 1082

// Page Performance controls (IDD 141)
#define IDC_PERF_IN_SLIDER          1092
#define IDC_PERF_OUT_SLIDER         1093
#define IDC_PERF_QUALITY            1094

// Page Menu controls (IDD 147)
#define IDC_MNU_HIDE_EXT            1004
#define IDC_MNU_ONLY_REG            1006
#define IDC_MNU_SHOW_SETTINGS_ALL   1011
#define IDC_MNU_SHOW_EXPLORER       1124
#define IDC_MNU_EXPLORER_LABEL      1125
