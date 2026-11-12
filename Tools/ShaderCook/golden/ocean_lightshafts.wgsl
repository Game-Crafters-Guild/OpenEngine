struct type_11 {
    member: mat4x4<f32>,
    member_1: vec4<f32>,
    member_2: vec4<f32>,
    member_3: vec4<f32>,
    member_4: vec4<f32>,
    member_5: vec4<f32>,
    member_6: vec4<f32>,
    member_7: vec4<f32>,
    member_8: vec4<f32>,
    member_9: array<vec4<f32>, 7>,
    member_10: vec4<f32>,
    member_11: vec4<f32>,
    member_12: array<vec4<f32>, 32>,
}

@group(0) @binding(1)
var<uniform> global: type_11;
@group(0) @binding(75)
var global_1: sampler;
@group(0) @binding(11)
var global_2: texture_2d_array<f32>;
@group(0) @binding(80)
var global_3: sampler;
@group(0) @binding(16)
var global_4: texture_2d_array<f32>;
@group(0) @binding(69)
var global_5: sampler;
@group(0) @binding(5)
var global_6: texture_2d_array<f32>;
@group(0) @binding(70)
var global_7: sampler;
@group(0) @binding(6)
var global_8: texture_2d_array<f32>;
@group(0) @binding(71)
var global_9: sampler;
@group(0) @binding(7)
var global_10: texture_2d_array<f32>;
@group(0) @binding(74)
var global_11: sampler;
@group(0) @binding(10)
var global_12: texture_2d_array<f32>;
@group(0) @binding(76)
var global_13: sampler;
@group(0) @binding(12)
var global_14: texture_2d_array<f32>;
@group(0) @binding(77)
var global_15: sampler;
@group(0) @binding(13)
var global_16: texture_2d_array<f32>;
@group(0) @binding(78)
var global_17: sampler;
@group(0) @binding(14)
var global_18: texture_2d_array<f32>;
@group(0) @binding(79)
var global_19: sampler;
@group(0) @binding(15)
var global_20: texture_2d_array<f32>;
@group(0) @binding(68)
var global_21: sampler;
@group(0) @binding(4)
var global_22: texture_2d_array<f32>;
@group(0) @binding(64)
var global_23: sampler;
@group(0) @binding(0)
var global_24: texture_2d<f32>;
var<private> global_25: vec2<f32>;
@group(0) @binding(66)
var global_26: sampler;
@group(0) @binding(2)
var global_27: texture_2d<f32>;
var<private> global_28: vec4<f32>;
@group(0) @binding(67)
var global_29: sampler;
@group(0) @binding(3)
var global_30: texture_2d<f32>;

fn function_() {
    var phi_911_: bool;
    var phi_921_: bool;
    var phi_1273_: bool;
    var phi_18825_: f32;
    var phi_18824_: i32;
    var phi_18552_: f32;
    var phi_18551_: i32;
    var phi_31905_: f32;
    var phi_18553_: i32;
    var phi_1659_: bool;
    var phi_18561_: vec4<f32>;
    var phi_18556_: bool;
    var phi_18560_: vec4<f32>;
    var phi_1295_: bool;
    var phi_18577_: f32;
    var phi_18576_: i32;
    var phi_31907_: f32;
    var local: f32;
    var local_1: f32;
    var phi_18579_: f32;
    var phi_1313_: bool;
    var phi_18581_: f32;
    var phi_18580_: i32;
    var phi_31908_: f32;
    var local_2: f32;
    var phi_18583_: f32;
    var phi_1331_: bool;
    var phi_18585_: f32;
    var phi_18584_: i32;
    var phi_31909_: f32;
    var local_3: f32;
    var phi_18587_: f32;
    var phi_1349_: bool;
    var phi_18589_: f32;
    var phi_18588_: i32;
    var phi_31910_: f32;
    var local_4: f32;
    var phi_18625_: f32;
    var local_5: bool;
    var phi_18590_: i32;
    var phi_2326_: bool;
    var phi_18598_: vec4<f32>;
    var phi_18593_: bool;
    var phi_18597_: vec4<f32>;
    var phi_1369_: bool;
    var phi_18632_: f32;
    var phi_18631_: i32;
    var phi_31944_: f32;
    var local_6: f32;
    var phi_18634_: f32;
    var phi_1387_: bool;
    var phi_18636_: f32;
    var phi_18635_: i32;
    var phi_31945_: f32;
    var local_7: f32;
    var phi_18638_: f32;
    var phi_1405_: bool;
    var phi_18640_: f32;
    var phi_18639_: i32;
    var phi_31946_: f32;
    var local_8: f32;
    var phi_18642_: f32;
    var phi_1423_: bool;
    var phi_18644_: f32;
    var phi_18643_: i32;
    var phi_31947_: f32;
    var local_9: f32;
    var phi_18646_: f32;
    var phi_18645_: f32;
    var phi_18826_: f32;
    var local_10: bool;
    var phi_3021_: bool;
    var phi_19312_: f32;
    var phi_19311_: i32;
    var phi_18828_: f32;
    var phi_18827_: i32;
    var phi_31948_: f32;
    var phi_18829_: i32;
    var phi_3407_: bool;
    var phi_18837_: vec4<f32>;
    var phi_18832_: bool;
    var phi_18836_: vec4<f32>;
    var phi_3043_: bool;
    var phi_18958_: f32;
    var phi_18957_: i32;
    var phi_31950_: f32;
    var local_11: f32;
    var local_12: f32;
    var phi_18960_: f32;
    var phi_3061_: bool;
    var phi_18962_: f32;
    var phi_18961_: i32;
    var phi_31951_: f32;
    var local_13: f32;
    var phi_18964_: f32;
    var phi_3079_: bool;
    var phi_18966_: f32;
    var phi_18965_: i32;
    var phi_31952_: f32;
    var local_14: f32;
    var phi_18968_: f32;
    var phi_3097_: bool;
    var phi_18970_: f32;
    var phi_18969_: i32;
    var phi_31953_: f32;
    var local_15: f32;
    var phi_19112_: f32;
    var local_16: bool;
    var phi_18971_: i32;
    var phi_4074_: bool;
    var phi_18979_: vec4<f32>;
    var phi_18974_: bool;
    var phi_18978_: vec4<f32>;
    var phi_3117_: bool;
    var phi_19119_: f32;
    var phi_19118_: i32;
    var phi_31987_: f32;
    var local_17: f32;
    var phi_19121_: f32;
    var phi_3135_: bool;
    var phi_19123_: f32;
    var phi_19122_: i32;
    var phi_31988_: f32;
    var local_18: f32;
    var phi_19125_: f32;
    var phi_3153_: bool;
    var phi_19127_: f32;
    var phi_19126_: i32;
    var phi_31989_: f32;
    var local_19: f32;
    var phi_19129_: f32;
    var phi_3171_: bool;
    var phi_19131_: f32;
    var phi_19130_: i32;
    var phi_31990_: f32;
    var local_20: f32;
    var phi_19133_: f32;
    var phi_19132_: f32;
    var phi_19313_: f32;
    var local_21: bool;
    var phi_4720_: bool;
    var phi_20011_: f32;
    var phi_20010_: i32;
    var phi_19315_: f32;
    var phi_19314_: i32;
    var phi_31991_: f32;
    var phi_19316_: i32;
    var phi_5106_: bool;
    var phi_19324_: vec4<f32>;
    var phi_19319_: bool;
    var phi_19323_: vec4<f32>;
    var phi_4742_: bool;
    var phi_19551_: f32;
    var phi_19550_: i32;
    var phi_31993_: f32;
    var local_22: f32;
    var local_23: f32;
    var phi_19553_: f32;
    var phi_4760_: bool;
    var phi_19555_: f32;
    var phi_19554_: i32;
    var phi_31994_: f32;
    var local_24: f32;
    var phi_19557_: f32;
    var phi_4778_: bool;
    var phi_19559_: f32;
    var phi_19558_: i32;
    var phi_31995_: f32;
    var local_25: f32;
    var phi_19561_: f32;
    var phi_4796_: bool;
    var phi_19563_: f32;
    var phi_19562_: i32;
    var phi_31996_: f32;
    var local_26: f32;
    var phi_19811_: f32;
    var local_27: bool;
    var phi_19564_: i32;
    var phi_5773_: bool;
    var phi_19572_: vec4<f32>;
    var phi_19567_: bool;
    var phi_19571_: vec4<f32>;
    var phi_4816_: bool;
    var phi_19818_: f32;
    var phi_19817_: i32;
    var phi_32030_: f32;
    var local_28: f32;
    var phi_19820_: f32;
    var phi_4834_: bool;
    var phi_19822_: f32;
    var phi_19821_: i32;
    var phi_32031_: f32;
    var local_29: f32;
    var phi_19824_: f32;
    var phi_4852_: bool;
    var phi_19826_: f32;
    var phi_19825_: i32;
    var phi_32032_: f32;
    var local_30: f32;
    var phi_19828_: f32;
    var phi_4870_: bool;
    var phi_19830_: f32;
    var phi_19829_: i32;
    var phi_32033_: f32;
    var local_31: f32;
    var phi_19832_: f32;
    var phi_19831_: f32;
    var phi_20012_: f32;
    var local_32: bool;
    var phi_6419_: bool;
    var phi_20922_: f32;
    var phi_20921_: i32;
    var phi_20014_: f32;
    var phi_20013_: i32;
    var phi_32034_: f32;
    var phi_20015_: i32;
    var phi_6805_: bool;
    var phi_20023_: vec4<f32>;
    var phi_20018_: bool;
    var phi_20022_: vec4<f32>;
    var phi_6441_: bool;
    var phi_20356_: f32;
    var phi_20355_: i32;
    var phi_32036_: f32;
    var local_33: f32;
    var local_34: f32;
    var phi_20358_: f32;
    var phi_6459_: bool;
    var phi_20360_: f32;
    var phi_20359_: i32;
    var phi_32037_: f32;
    var local_35: f32;
    var phi_20362_: f32;
    var phi_6477_: bool;
    var phi_20364_: f32;
    var phi_20363_: i32;
    var phi_32038_: f32;
    var local_36: f32;
    var phi_20366_: f32;
    var phi_6495_: bool;
    var phi_20368_: f32;
    var phi_20367_: i32;
    var phi_32039_: f32;
    var local_37: f32;
    var phi_20722_: f32;
    var local_38: bool;
    var phi_20369_: i32;
    var phi_7472_: bool;
    var phi_20377_: vec4<f32>;
    var phi_20372_: bool;
    var phi_20376_: vec4<f32>;
    var phi_6515_: bool;
    var phi_20729_: f32;
    var phi_20728_: i32;
    var phi_32073_: f32;
    var local_39: f32;
    var phi_20731_: f32;
    var phi_6533_: bool;
    var phi_20733_: f32;
    var phi_20732_: i32;
    var phi_32074_: f32;
    var local_40: f32;
    var phi_20735_: f32;
    var phi_6551_: bool;
    var phi_20737_: f32;
    var phi_20736_: i32;
    var phi_32075_: f32;
    var local_41: f32;
    var phi_20739_: f32;
    var phi_6569_: bool;
    var phi_20741_: f32;
    var phi_20740_: i32;
    var phi_32076_: f32;
    var local_42: f32;
    var phi_20743_: f32;
    var phi_20742_: f32;
    var phi_20923_: f32;
    var local_43: bool;
    var phi_8118_: bool;
    var phi_22045_: f32;
    var phi_22044_: i32;
    var phi_20925_: f32;
    var phi_20924_: i32;
    var phi_32077_: f32;
    var phi_20926_: i32;
    var phi_8504_: bool;
    var phi_20934_: vec4<f32>;
    var phi_20929_: bool;
    var phi_20933_: vec4<f32>;
    var phi_8140_: bool;
    var phi_21373_: f32;
    var phi_21372_: i32;
    var phi_32079_: f32;
    var local_44: f32;
    var local_45: f32;
    var phi_21375_: f32;
    var phi_8158_: bool;
    var phi_21377_: f32;
    var phi_21376_: i32;
    var phi_32080_: f32;
    var local_46: f32;
    var phi_21379_: f32;
    var phi_8176_: bool;
    var phi_21381_: f32;
    var phi_21380_: i32;
    var phi_32081_: f32;
    var local_47: f32;
    var phi_21383_: f32;
    var phi_8194_: bool;
    var phi_21385_: f32;
    var phi_21384_: i32;
    var phi_32082_: f32;
    var local_48: f32;
    var phi_21845_: f32;
    var local_49: bool;
    var phi_21386_: i32;
    var phi_9171_: bool;
    var phi_21394_: vec4<f32>;
    var phi_21389_: bool;
    var phi_21393_: vec4<f32>;
    var phi_8214_: bool;
    var phi_21852_: f32;
    var phi_21851_: i32;
    var phi_32116_: f32;
    var local_50: f32;
    var phi_21854_: f32;
    var phi_8232_: bool;
    var phi_21856_: f32;
    var phi_21855_: i32;
    var phi_32117_: f32;
    var local_51: f32;
    var phi_21858_: f32;
    var phi_8250_: bool;
    var phi_21860_: f32;
    var phi_21859_: i32;
    var phi_32118_: f32;
    var local_52: f32;
    var phi_21862_: f32;
    var phi_8268_: bool;
    var phi_21864_: f32;
    var phi_21863_: i32;
    var phi_32119_: f32;
    var local_53: f32;
    var phi_21866_: f32;
    var phi_21865_: f32;
    var phi_22046_: f32;
    var local_54: bool;
    var phi_9817_: bool;
    var phi_23380_: f32;
    var phi_23379_: i32;
    var phi_22048_: f32;
    var phi_22047_: i32;
    var phi_32120_: f32;
    var phi_22049_: i32;
    var phi_10203_: bool;
    var phi_22057_: vec4<f32>;
    var phi_22052_: bool;
    var phi_22056_: vec4<f32>;
    var phi_9839_: bool;
    var phi_22602_: f32;
    var phi_22601_: i32;
    var phi_32122_: f32;
    var local_55: f32;
    var local_56: f32;
    var phi_22604_: f32;
    var phi_9857_: bool;
    var phi_22606_: f32;
    var phi_22605_: i32;
    var phi_32123_: f32;
    var local_57: f32;
    var phi_22608_: f32;
    var phi_9875_: bool;
    var phi_22610_: f32;
    var phi_22609_: i32;
    var phi_32124_: f32;
    var local_58: f32;
    var phi_22612_: f32;
    var phi_9893_: bool;
    var phi_22614_: f32;
    var phi_22613_: i32;
    var phi_32125_: f32;
    var local_59: f32;
    var phi_23180_: f32;
    var local_60: bool;
    var phi_22615_: i32;
    var phi_10870_: bool;
    var phi_22623_: vec4<f32>;
    var phi_22618_: bool;
    var phi_22622_: vec4<f32>;
    var phi_9913_: bool;
    var phi_23187_: f32;
    var phi_23186_: i32;
    var phi_32159_: f32;
    var local_61: f32;
    var phi_23189_: f32;
    var phi_9931_: bool;
    var phi_23191_: f32;
    var phi_23190_: i32;
    var phi_32160_: f32;
    var local_62: f32;
    var phi_23193_: f32;
    var phi_9949_: bool;
    var phi_23195_: f32;
    var phi_23194_: i32;
    var phi_32161_: f32;
    var local_63: f32;
    var phi_23197_: f32;
    var phi_9967_: bool;
    var phi_23199_: f32;
    var phi_23198_: i32;
    var phi_32162_: f32;
    var local_64: f32;
    var phi_23201_: f32;
    var phi_23200_: f32;
    var phi_23381_: f32;
    var local_65: bool;
    var phi_11565_: bool;
    var phi_24927_: f32;
    var phi_24926_: i32;
    var phi_23383_: f32;
    var phi_23382_: i32;
    var phi_32163_: f32;
    var phi_23384_: i32;
    var phi_11951_: bool;
    var phi_23392_: vec4<f32>;
    var phi_23387_: bool;
    var phi_23391_: vec4<f32>;
    var phi_11587_: bool;
    var phi_24043_: f32;
    var phi_24042_: i32;
    var phi_32165_: f32;
    var local_66: f32;
    var local_67: f32;
    var phi_24045_: f32;
    var phi_11605_: bool;
    var phi_24047_: f32;
    var phi_24046_: i32;
    var phi_32166_: f32;
    var local_68: f32;
    var phi_24049_: f32;
    var phi_11623_: bool;
    var phi_24051_: f32;
    var phi_24050_: i32;
    var phi_32167_: f32;
    var local_69: f32;
    var phi_24053_: f32;
    var phi_11641_: bool;
    var phi_24055_: f32;
    var phi_24054_: i32;
    var phi_32168_: f32;
    var local_70: f32;
    var phi_24727_: f32;
    var local_71: bool;
    var phi_24056_: i32;
    var phi_12618_: bool;
    var phi_24064_: vec4<f32>;
    var phi_24059_: bool;
    var phi_24063_: vec4<f32>;
    var phi_11661_: bool;
    var phi_24734_: f32;
    var phi_24733_: i32;
    var phi_32202_: f32;
    var local_72: f32;
    var phi_24736_: f32;
    var phi_11679_: bool;
    var phi_24738_: f32;
    var phi_24737_: i32;
    var phi_32203_: f32;
    var local_73: f32;
    var phi_24740_: f32;
    var phi_11697_: bool;
    var phi_24742_: f32;
    var phi_24741_: i32;
    var phi_32204_: f32;
    var local_74: f32;
    var phi_24744_: f32;
    var phi_11715_: bool;
    var phi_24746_: f32;
    var phi_24745_: i32;
    var phi_32205_: f32;
    var local_75: f32;
    var phi_24748_: f32;
    var phi_24747_: f32;
    var phi_24928_: f32;
    var local_76: bool;
    var phi_13264_: bool;
    var phi_26686_: f32;
    var phi_26685_: i32;
    var phi_24930_: f32;
    var phi_24929_: i32;
    var phi_32206_: f32;
    var phi_24931_: i32;
    var phi_13650_: bool;
    var phi_24939_: vec4<f32>;
    var phi_24934_: bool;
    var phi_24938_: vec4<f32>;
    var phi_13286_: bool;
    var phi_25696_: f32;
    var phi_25695_: i32;
    var phi_32208_: f32;
    var local_77: f32;
    var local_78: f32;
    var phi_25698_: f32;
    var phi_13304_: bool;
    var phi_25700_: f32;
    var phi_25699_: i32;
    var phi_32209_: f32;
    var local_79: f32;
    var phi_25702_: f32;
    var phi_13322_: bool;
    var phi_25704_: f32;
    var phi_25703_: i32;
    var phi_32210_: f32;
    var local_80: f32;
    var phi_25706_: f32;
    var phi_13340_: bool;
    var phi_25708_: f32;
    var phi_25707_: i32;
    var phi_32211_: f32;
    var local_81: f32;
    var phi_26486_: f32;
    var local_82: bool;
    var phi_25709_: i32;
    var phi_14317_: bool;
    var phi_25717_: vec4<f32>;
    var phi_25712_: bool;
    var phi_25716_: vec4<f32>;
    var phi_13360_: bool;
    var phi_26493_: f32;
    var phi_26492_: i32;
    var phi_32245_: f32;
    var local_83: f32;
    var phi_26495_: f32;
    var phi_13378_: bool;
    var phi_26497_: f32;
    var phi_26496_: i32;
    var phi_32246_: f32;
    var local_84: f32;
    var phi_26499_: f32;
    var phi_13396_: bool;
    var phi_26501_: f32;
    var phi_26500_: i32;
    var phi_32247_: f32;
    var local_85: f32;
    var phi_26503_: f32;
    var phi_13414_: bool;
    var phi_26505_: f32;
    var phi_26504_: i32;
    var phi_32248_: f32;
    var local_86: f32;
    var phi_26507_: f32;
    var phi_26506_: f32;
    var phi_26687_: f32;
    var local_87: bool;
    var phi_14963_: bool;
    var phi_28657_: f32;
    var phi_28656_: i32;
    var phi_26689_: f32;
    var phi_26688_: i32;
    var phi_32249_: f32;
    var phi_26690_: i32;
    var phi_15349_: bool;
    var phi_26698_: vec4<f32>;
    var phi_26693_: bool;
    var phi_26697_: vec4<f32>;
    var phi_14985_: bool;
    var phi_27561_: f32;
    var phi_27560_: i32;
    var phi_32251_: f32;
    var local_88: f32;
    var local_89: f32;
    var phi_27563_: f32;
    var phi_15003_: bool;
    var phi_27565_: f32;
    var phi_27564_: i32;
    var phi_32252_: f32;
    var local_90: f32;
    var phi_27567_: f32;
    var phi_15021_: bool;
    var phi_27569_: f32;
    var phi_27568_: i32;
    var phi_32253_: f32;
    var local_91: f32;
    var phi_27571_: f32;
    var phi_15039_: bool;
    var phi_27573_: f32;
    var phi_27572_: i32;
    var phi_32254_: f32;
    var local_92: f32;
    var phi_28457_: f32;
    var local_93: bool;
    var phi_27574_: i32;
    var phi_16016_: bool;
    var phi_27582_: vec4<f32>;
    var phi_27577_: bool;
    var phi_27581_: vec4<f32>;
    var phi_15059_: bool;
    var phi_28464_: f32;
    var phi_28463_: i32;
    var phi_32288_: f32;
    var local_94: f32;
    var phi_28466_: f32;
    var phi_15077_: bool;
    var phi_28468_: f32;
    var phi_28467_: i32;
    var phi_32289_: f32;
    var local_95: f32;
    var phi_28470_: f32;
    var phi_15095_: bool;
    var phi_28472_: f32;
    var phi_28471_: i32;
    var phi_32290_: f32;
    var local_96: f32;
    var phi_28474_: f32;
    var phi_15113_: bool;
    var phi_28476_: f32;
    var phi_28475_: i32;
    var phi_32291_: f32;
    var local_97: f32;
    var phi_28478_: f32;
    var phi_28477_: f32;
    var phi_28658_: f32;
    var local_98: bool;
    var phi_16662_: bool;
    var phi_30840_: f32;
    var phi_30839_: i32;
    var phi_28660_: f32;
    var phi_28659_: i32;
    var phi_32292_: f32;
    var phi_28661_: i32;
    var phi_17048_: bool;
    var phi_28669_: vec4<f32>;
    var phi_28664_: bool;
    var phi_28668_: vec4<f32>;
    var phi_16684_: bool;
    var phi_29638_: f32;
    var phi_29637_: i32;
    var phi_32294_: f32;
    var local_99: f32;
    var local_100: f32;
    var phi_29640_: f32;
    var phi_16702_: bool;
    var phi_29642_: f32;
    var phi_29641_: i32;
    var phi_32295_: f32;
    var local_101: f32;
    var phi_29644_: f32;
    var phi_16720_: bool;
    var phi_29646_: f32;
    var phi_29645_: i32;
    var phi_32296_: f32;
    var local_102: f32;
    var phi_29648_: f32;
    var phi_16738_: bool;
    var phi_29650_: f32;
    var phi_29649_: i32;
    var phi_32297_: f32;
    var local_103: f32;
    var phi_30640_: f32;
    var local_104: bool;
    var phi_29651_: i32;
    var phi_17715_: bool;
    var phi_29659_: vec4<f32>;
    var phi_29654_: bool;
    var phi_29658_: vec4<f32>;
    var phi_16758_: bool;
    var phi_30647_: f32;
    var phi_30646_: i32;
    var phi_32331_: f32;
    var local_105: f32;
    var phi_30649_: f32;
    var phi_16776_: bool;
    var phi_30651_: f32;
    var phi_30650_: i32;
    var phi_32332_: f32;
    var local_106: f32;
    var phi_30653_: f32;
    var phi_16794_: bool;
    var phi_30655_: f32;
    var phi_30654_: i32;
    var phi_32333_: f32;
    var local_107: f32;
    var phi_30657_: f32;
    var phi_16812_: bool;
    var phi_30659_: f32;
    var phi_30658_: i32;
    var phi_32334_: f32;
    var local_108: f32;
    var phi_30661_: f32;
    var phi_30660_: f32;
    var phi_30841_: f32;
    var phi_31904_: vec3<f32>;
    var local_109: f32;
    var local_110: f32;
    var local_111: f32;
    var local_112: bool;
    var local_113: f32;
    var local_114: f32;
    var local_115: f32;
    var local_116: bool;
    var local_117: f32;
    var local_118: f32;
    var local_119: f32;
    var local_120: bool;
    var local_121: f32;
    var local_122: f32;
    var local_123: f32;
    var local_124: bool;
    var local_125: f32;
    var local_126: f32;
    var local_127: f32;
    var local_128: bool;
    var local_129: f32;
    var local_130: f32;
    var local_131: f32;
    var local_132: bool;
    var local_133: f32;
    var local_134: f32;
    var local_135: f32;
    var local_136: bool;
    var local_137: f32;
    var local_138: f32;
    var local_139: f32;
    var local_140: bool;
    var local_141: f32;
    var local_142: f32;
    var local_143: f32;
    var local_144: bool;
    var local_145: f32;
    var local_146: f32;
    var local_147: f32;

    switch bitcast<i32>(0u) {
        default: {
            let _e98 = global_25;
            let _e99 = textureSample(global_24, global_23, _e98);
            let _e100 = _e99.xyz;
            let _e101 = textureSample(global_27, global_26, _e98);
            let _e112 = global.member;
            let _e113 = (_e112 * vec4<f32>(((_e98.x * 2f) - 1f), (((1f - _e98.y) * 2f) - 1f), _e101.x, 1f));
            let _e119 = (_e113.xyz / vec3(max(abs(_e113.w), 0.00001f)));
            let _e120 = dpdx(_e119);
            let _e121 = dpdy(_e119);
            let _e125 = global.member_4[0u];
            let _e126 = (_e119.y - _e125);
            let _e129 = global.member_4[2u];
            let _e131 = (_e126 > (0.08f - _e129));
            phi_911_ = _e131;
            if _e131 {
                let _e134 = global.member_6[1u];
                phi_911_ = (_e126 < (max(_e134, 0.001f) + _e129));
            }
            let _e139 = phi_911_;
            let _e142 = global.member_4[1u];
            let _e143 = (_e142 < 0.5f);
            phi_921_ = _e143;
            if !(_e143) {
                phi_921_ = (_e101.x <= 0f);
            }
            let _e147 = phi_921_;
            if (_e147 || !(_e139)) {
                global_28 = vec4<f32>(_e99.x, _e99.y, _e99.z, 1f);
                break;
            }
            let _e159 = (1f - smoothstep(0.28f, 0.72f, abs(normalize(cross(_e120, _e121)).y)));
            let _e161 = global.member_1;
            let _e164 = length((_e119 - _e161.xyz));
            let _e166 = global.member_3;
            let _e168 = normalize(_e166.xyz);
            let _e169 = _e119.xz;
            switch bitcast<i32>(0u) {
                default: {
                    let _e173 = global.member_7[0u];
                    let _e174 = (_e173 < 0.5f);
                    let _e175 = !(_e174);
                    phi_1273_ = _e174;
                    local_10 = _e175;
                    local_112 = _e174;
                    local_21 = _e175;
                    local_116 = _e174;
                    local_32 = _e175;
                    local_120 = _e174;
                    local_43 = _e175;
                    local_124 = _e174;
                    local_54 = _e175;
                    local_128 = _e174;
                    local_65 = _e175;
                    local_132 = _e174;
                    local_76 = _e175;
                    local_136 = _e174;
                    local_87 = _e175;
                    local_140 = _e174;
                    local_98 = _e175;
                    local_144 = _e174;
                    if _e175 {
                        let _e178 = global.member_7[1u];
                        phi_1273_ = (_e178 < 0.5f);
                    }
                    let _e181 = phi_1273_;
                    if _e181 {
                        let _e184 = global.member_11[0u];
                        phi_18825_ = _e125;
                        phi_18824_ = 0i;
                        loop {
                            let _e188 = phi_18825_;
                            let _e190 = phi_18824_;
                            local_111 = _e188;
                            if (_e190 < i32(min(_e184, 16f))) {
                                continue;
                            } else {
                                break;
                            }
                            continuing {
                                let _e192 = (_e190 * 2i);
                                let _e195 = global.member_12[_e192];
                                let _e199 = global.member_12[(_e192 + 1i)];
                                let _e200 = _e195.xy;
                                let _e206 = max(_e195.w, 0.001f);
                                let _e207 = (6.2831855f / _e206);
                                let _e216 = global.member_2[0u];
                                phi_18825_ = (_e188 + (_e195.z * sin(((_e207 * dot((_e200 / vec2(max(length(_e200), 0.0001f))), _e169)) + (_e216 * (_e207 * (sqrt((1.56131f * _e206)) * _e199.y)))))));
                                phi_18824_ = (_e190 + 1i);
                            }
                        }
                        let _e6451 = local_111;
                        phi_18826_ = _e6451;
                        break;
                    }
                    let _e227 = global.member_7[1u];
                    let _e229 = i32(min(_e227, 16f));
                    phi_18552_ = 0f;
                    phi_18551_ = 0i;
                    loop {
                        let _e231 = phi_18552_;
                        let _e233 = phi_18551_;
                        local = _e231;
                        local_109 = _e231;
                        local_110 = _e231;
                        if (_e233 < _e229) {
                            let _e237 = f32((1i << bitcast<u32>(_e233)));
                            let _e242 = (1f - smoothstep((_e237 * 15f), (_e237 * 30f), _e164));
                            if (_e242 <= 0f) {
                                phi_31905_ = _e231;
                                continue;
                            }
                            let _e245 = (_e169 / vec2((0.5f * _e237)));
                            let _e249 = vec3<f32>(_e245.x, _e245.y, f32(_e233));
                            let _e255 = textureSampleLevel(global_22, global_21, vec2<f32>(_e249.x, _e249.y), i32(_e249.z), 0f);
                            phi_31905_ = (_e231 + (_e255.y * _e242));
                            continue;
                        } else {
                            break;
                        }
                        continuing {
                            let _e260 = phi_31905_;
                            phi_18552_ = _e260;
                            phi_18551_ = (_e233 + 1i);
                        }
                    }
                    let _e264 = global.member_8[0u];
                    let _e267 = u32((max(_e264, 0f) + 0.5f));
                    let _e6448 = local_110;
                    phi_18645_ = _e6448;
                    if (_e267 != 0u) {
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e270 = (_e267 == 0u);
                                local_5 = _e270;
                                if _e270 {
                                    phi_18560_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e273 = global.member_10[0u];
                                let _e276 = min(i32((_e273 + 0.5f)), 7i);
                                if (_e276 <= 0i) {
                                    phi_18560_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e278 = textureDimensions(global_2, 0i);
                                let _e281 = f32(vec2<i32>(_e278).x);
                                phi_18553_ = 0i;
                                loop {
                                    let _e283 = phi_18553_;
                                    phi_18561_ = vec4<f32>();
                                    phi_18556_ = false;
                                    if (_e283 < 7i) {
                                        if (_e283 >= _e276) {
                                            phi_18561_ = vec4<f32>();
                                            phi_18556_ = false;
                                            break;
                                        }
                                        let _e288 = global.member_9[_e283];
                                        let _e294 = ((_e169 - _e288.xy) / vec2((_e288.z * _e281)));
                                        let _e296 = all((_e294 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_1659_ = _e296;
                                        if _e296 {
                                            phi_1659_ = all((_e294 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e300 = phi_1659_;
                                        if _e300 {
                                            let _e304 = vec3<f32>(_e294.x, _e294.y, f32(_e283));
                                            let _e310 = textureSampleLevel(global_2, global_1, vec2<f32>(_e304.x, _e304.y), i32(_e304.z), 0f);
                                            phi_18561_ = clamp(_e310, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_18556_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_18553_ = (_e283 + 1i);
                                    }
                                }
                                let _e314 = phi_18561_;
                                let _e316 = phi_18556_;
                                phi_18560_ = _e314;
                                if _e316 {
                                    break;
                                }
                                let _e317 = (_e276 - 1i);
                                let _e320 = global.member_9[_e317];
                                let _e327 = clamp(((_e169 - _e320.xy) / vec2((_e320.z * _e281))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e331 = vec3<f32>(_e327.x, _e327.y, f32(_e317));
                                let _e337 = textureSampleLevel(global_2, global_1, vec2<f32>(_e331.x, _e331.y), i32(_e331.z), 0f);
                                phi_18560_ = clamp(_e337, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e340 = phi_18560_;
                        let _e342 = ((_e267 & 1u) != 0u);
                        phi_1295_ = _e342;
                        if _e342 {
                            phi_1295_ = (_e340.x > 0.0001f);
                        }
                        let _e346 = phi_1295_;
                        let _e6397 = local_109;
                        phi_18579_ = _e6397;
                        if _e346 {
                            phi_18577_ = 0f;
                            phi_18576_ = 0i;
                            loop {
                                let _e348 = phi_18577_;
                                let _e350 = phi_18576_;
                                local_1 = _e348;
                                if (_e350 < _e229) {
                                    let _e354 = f32((1i << bitcast<u32>(_e350)));
                                    let _e359 = (1f - smoothstep((_e354 * 15f), (_e354 * 30f), _e164));
                                    if (_e359 <= 0f) {
                                        phi_31907_ = _e348;
                                        continue;
                                    }
                                    let _e362 = (_e169 / vec2((0.5f * _e354)));
                                    let _e366 = vec3<f32>(_e362.x, _e362.y, f32(_e350));
                                    let _e372 = textureSampleLevel(global_6, global_5, vec2<f32>(_e366.x, _e366.y), i32(_e366.z), 0f);
                                    phi_31907_ = (_e348 + (_e372.y * _e359));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e377 = phi_31907_;
                                    phi_18577_ = _e377;
                                    phi_18576_ = (_e350 + 1i);
                                }
                            }
                            let _e381 = local;
                            let _e383 = local_1;
                            phi_18579_ = mix(_e381, _e383, _e340.x);
                        }
                        let _e386 = phi_18579_;
                        let _e388 = ((_e267 & 2u) != 0u);
                        phi_1313_ = _e388;
                        if _e388 {
                            phi_1313_ = (_e340.y > 0.0001f);
                        }
                        let _e392 = phi_1313_;
                        phi_18583_ = _e386;
                        if _e392 {
                            phi_18581_ = 0f;
                            phi_18580_ = 0i;
                            loop {
                                let _e394 = phi_18581_;
                                let _e396 = phi_18580_;
                                local_2 = _e394;
                                if (_e396 < _e229) {
                                    let _e400 = f32((1i << bitcast<u32>(_e396)));
                                    let _e405 = (1f - smoothstep((_e400 * 15f), (_e400 * 30f), _e164));
                                    if (_e405 <= 0f) {
                                        phi_31908_ = _e394;
                                        continue;
                                    }
                                    let _e408 = (_e169 / vec2((0.5f * _e400)));
                                    let _e412 = vec3<f32>(_e408.x, _e408.y, f32(_e396));
                                    let _e418 = textureSampleLevel(global_8, global_7, vec2<f32>(_e412.x, _e412.y), i32(_e412.z), 0f);
                                    phi_31908_ = (_e394 + (_e418.y * _e405));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e423 = phi_31908_;
                                    phi_18581_ = _e423;
                                    phi_18580_ = (_e396 + 1i);
                                }
                            }
                            let _e427 = local_2;
                            phi_18583_ = mix(_e386, _e427, _e340.y);
                        }
                        let _e430 = phi_18583_;
                        let _e432 = ((_e267 & 4u) != 0u);
                        phi_1331_ = _e432;
                        if _e432 {
                            phi_1331_ = (_e340.z > 0.0001f);
                        }
                        let _e436 = phi_1331_;
                        phi_18587_ = _e430;
                        if _e436 {
                            phi_18585_ = 0f;
                            phi_18584_ = 0i;
                            loop {
                                let _e438 = phi_18585_;
                                let _e440 = phi_18584_;
                                local_3 = _e438;
                                if (_e440 < _e229) {
                                    let _e444 = f32((1i << bitcast<u32>(_e440)));
                                    let _e449 = (1f - smoothstep((_e444 * 15f), (_e444 * 30f), _e164));
                                    if (_e449 <= 0f) {
                                        phi_31909_ = _e438;
                                        continue;
                                    }
                                    let _e452 = (_e169 / vec2((0.5f * _e444)));
                                    let _e456 = vec3<f32>(_e452.x, _e452.y, f32(_e440));
                                    let _e462 = textureSampleLevel(global_10, global_9, vec2<f32>(_e456.x, _e456.y), i32(_e456.z), 0f);
                                    phi_31909_ = (_e438 + (_e462.y * _e449));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e467 = phi_31909_;
                                    phi_18585_ = _e467;
                                    phi_18584_ = (_e440 + 1i);
                                }
                            }
                            let _e471 = local_3;
                            phi_18587_ = mix(_e430, _e471, _e340.z);
                        }
                        let _e474 = phi_18587_;
                        let _e476 = ((_e267 & 8u) != 0u);
                        phi_1349_ = _e476;
                        if _e476 {
                            phi_1349_ = (_e340.w > 0.0001f);
                        }
                        let _e480 = phi_1349_;
                        phi_18625_ = _e474;
                        if _e480 {
                            phi_18589_ = 0f;
                            phi_18588_ = 0i;
                            loop {
                                let _e482 = phi_18589_;
                                let _e484 = phi_18588_;
                                local_4 = _e482;
                                if (_e484 < _e229) {
                                    let _e488 = f32((1i << bitcast<u32>(_e484)));
                                    let _e493 = (1f - smoothstep((_e488 * 15f), (_e488 * 30f), _e164));
                                    if (_e493 <= 0f) {
                                        phi_31910_ = _e482;
                                        continue;
                                    }
                                    let _e496 = (_e169 / vec2((0.5f * _e488)));
                                    let _e500 = vec3<f32>(_e496.x, _e496.y, f32(_e484));
                                    let _e506 = textureSampleLevel(global_12, global_11, vec2<f32>(_e500.x, _e500.y), i32(_e500.z), 0f);
                                    phi_31910_ = (_e482 + (_e506.y * _e493));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e511 = phi_31910_;
                                    phi_18589_ = _e511;
                                    phi_18588_ = (_e484 + 1i);
                                }
                            }
                            let _e515 = local_4;
                            phi_18625_ = mix(_e474, _e515, _e340.w);
                        }
                        let _e518 = phi_18625_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e521 = local_5;
                                if _e521 {
                                    phi_18597_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e524 = global.member_10[0u];
                                let _e527 = min(i32((_e524 + 0.5f)), 7i);
                                if (_e527 <= 0i) {
                                    phi_18597_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e529 = textureDimensions(global_2, 0i);
                                let _e532 = f32(vec2<i32>(_e529).x);
                                phi_18590_ = 0i;
                                loop {
                                    let _e534 = phi_18590_;
                                    phi_18598_ = vec4<f32>();
                                    phi_18593_ = false;
                                    if (_e534 < 7i) {
                                        if (_e534 >= _e527) {
                                            phi_18598_ = vec4<f32>();
                                            phi_18593_ = false;
                                            break;
                                        }
                                        let _e539 = global.member_9[_e534];
                                        let _e545 = ((_e169 - _e539.xy) / vec2((_e539.z * _e532)));
                                        let _e547 = all((_e545 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_2326_ = _e547;
                                        if _e547 {
                                            phi_2326_ = all((_e545 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e551 = phi_2326_;
                                        if _e551 {
                                            let _e555 = vec3<f32>(_e545.x, _e545.y, f32(_e534));
                                            let _e561 = textureSampleLevel(global_4, global_3, vec2<f32>(_e555.x, _e555.y), i32(_e555.z), 0f);
                                            phi_18598_ = clamp(_e561, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_18593_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_18590_ = (_e534 + 1i);
                                    }
                                }
                                let _e565 = phi_18598_;
                                let _e567 = phi_18593_;
                                phi_18597_ = _e565;
                                if _e567 {
                                    break;
                                }
                                let _e568 = (_e527 - 1i);
                                let _e571 = global.member_9[_e568];
                                let _e578 = clamp(((_e169 - _e571.xy) / vec2((_e571.z * _e532))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e582 = vec3<f32>(_e578.x, _e578.y, f32(_e568));
                                let _e588 = textureSampleLevel(global_4, global_3, vec2<f32>(_e582.x, _e582.y), i32(_e582.z), 0f);
                                phi_18597_ = clamp(_e588, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e591 = phi_18597_;
                        let _e593 = ((_e267 & 16u) != 0u);
                        phi_1369_ = _e593;
                        if _e593 {
                            phi_1369_ = (_e591.x > 0.0001f);
                        }
                        let _e597 = phi_1369_;
                        phi_18634_ = _e518;
                        if _e597 {
                            phi_18632_ = 0f;
                            phi_18631_ = 0i;
                            loop {
                                let _e599 = phi_18632_;
                                let _e601 = phi_18631_;
                                local_6 = _e599;
                                if (_e601 < _e229) {
                                    let _e605 = f32((1i << bitcast<u32>(_e601)));
                                    let _e610 = (1f - smoothstep((_e605 * 15f), (_e605 * 30f), _e164));
                                    if (_e610 <= 0f) {
                                        phi_31944_ = _e599;
                                        continue;
                                    }
                                    let _e613 = (_e169 / vec2((0.5f * _e605)));
                                    let _e617 = vec3<f32>(_e613.x, _e613.y, f32(_e601));
                                    let _e623 = textureSampleLevel(global_14, global_13, vec2<f32>(_e617.x, _e617.y), i32(_e617.z), 0f);
                                    phi_31944_ = (_e599 + (_e623.y * _e610));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e628 = phi_31944_;
                                    phi_18632_ = _e628;
                                    phi_18631_ = (_e601 + 1i);
                                }
                            }
                            let _e632 = local_6;
                            phi_18634_ = mix(_e518, _e632, _e591.x);
                        }
                        let _e635 = phi_18634_;
                        let _e637 = ((_e267 & 32u) != 0u);
                        phi_1387_ = _e637;
                        if _e637 {
                            phi_1387_ = (_e591.y > 0.0001f);
                        }
                        let _e641 = phi_1387_;
                        phi_18638_ = _e635;
                        if _e641 {
                            phi_18636_ = 0f;
                            phi_18635_ = 0i;
                            loop {
                                let _e643 = phi_18636_;
                                let _e645 = phi_18635_;
                                local_7 = _e643;
                                if (_e645 < _e229) {
                                    let _e649 = f32((1i << bitcast<u32>(_e645)));
                                    let _e654 = (1f - smoothstep((_e649 * 15f), (_e649 * 30f), _e164));
                                    if (_e654 <= 0f) {
                                        phi_31945_ = _e643;
                                        continue;
                                    }
                                    let _e657 = (_e169 / vec2((0.5f * _e649)));
                                    let _e661 = vec3<f32>(_e657.x, _e657.y, f32(_e645));
                                    let _e667 = textureSampleLevel(global_16, global_15, vec2<f32>(_e661.x, _e661.y), i32(_e661.z), 0f);
                                    phi_31945_ = (_e643 + (_e667.y * _e654));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e672 = phi_31945_;
                                    phi_18636_ = _e672;
                                    phi_18635_ = (_e645 + 1i);
                                }
                            }
                            let _e676 = local_7;
                            phi_18638_ = mix(_e635, _e676, _e591.y);
                        }
                        let _e679 = phi_18638_;
                        let _e681 = ((_e267 & 64u) != 0u);
                        phi_1405_ = _e681;
                        if _e681 {
                            phi_1405_ = (_e591.z > 0.0001f);
                        }
                        let _e685 = phi_1405_;
                        phi_18642_ = _e679;
                        if _e685 {
                            phi_18640_ = 0f;
                            phi_18639_ = 0i;
                            loop {
                                let _e687 = phi_18640_;
                                let _e689 = phi_18639_;
                                local_8 = _e687;
                                if (_e689 < _e229) {
                                    let _e693 = f32((1i << bitcast<u32>(_e689)));
                                    let _e698 = (1f - smoothstep((_e693 * 15f), (_e693 * 30f), _e164));
                                    if (_e698 <= 0f) {
                                        phi_31946_ = _e687;
                                        continue;
                                    }
                                    let _e701 = (_e169 / vec2((0.5f * _e693)));
                                    let _e705 = vec3<f32>(_e701.x, _e701.y, f32(_e689));
                                    let _e711 = textureSampleLevel(global_18, global_17, vec2<f32>(_e705.x, _e705.y), i32(_e705.z), 0f);
                                    phi_31946_ = (_e687 + (_e711.y * _e698));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e716 = phi_31946_;
                                    phi_18640_ = _e716;
                                    phi_18639_ = (_e689 + 1i);
                                }
                            }
                            let _e720 = local_8;
                            phi_18642_ = mix(_e679, _e720, _e591.z);
                        }
                        let _e723 = phi_18642_;
                        let _e725 = ((_e267 & 128u) != 0u);
                        phi_1423_ = _e725;
                        if _e725 {
                            phi_1423_ = (_e591.w > 0.0001f);
                        }
                        let _e729 = phi_1423_;
                        phi_18646_ = _e723;
                        if _e729 {
                            phi_18644_ = 0f;
                            phi_18643_ = 0i;
                            loop {
                                let _e731 = phi_18644_;
                                let _e733 = phi_18643_;
                                local_9 = _e731;
                                if (_e733 < _e229) {
                                    let _e737 = f32((1i << bitcast<u32>(_e733)));
                                    let _e742 = (1f - smoothstep((_e737 * 15f), (_e737 * 30f), _e164));
                                    if (_e742 <= 0f) {
                                        phi_31947_ = _e731;
                                        continue;
                                    }
                                    let _e745 = (_e169 / vec2((0.5f * _e737)));
                                    let _e749 = vec3<f32>(_e745.x, _e745.y, f32(_e733));
                                    let _e755 = textureSampleLevel(global_20, global_19, vec2<f32>(_e749.x, _e749.y), i32(_e749.z), 0f);
                                    phi_31947_ = (_e731 + (_e755.y * _e742));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e760 = phi_31947_;
                                    phi_18644_ = _e760;
                                    phi_18643_ = (_e733 + 1i);
                                }
                            }
                            let _e764 = local_9;
                            phi_18646_ = mix(_e723, _e764, _e591.w);
                        }
                        let _e767 = phi_18646_;
                        phi_18645_ = _e767;
                    }
                    let _e769 = phi_18645_;
                    let _e772 = global.member_7[2u];
                    let _e776 = global.member_7[3u];
                    phi_18826_ = (_e125 + clamp((_e769 * _e772), -(_e776), _e776));
                    break;
                }
            }
            let _e781 = phi_18826_;
            let _e784 = (_e169 - vec2<f32>(0.35f, 0f));
            switch bitcast<i32>(0u) {
                default: {
                    let _e787 = local_10;
                    let _e6455 = local_112;
                    phi_3021_ = _e6455;
                    if _e787 {
                        let _e790 = global.member_7[1u];
                        phi_3021_ = (_e790 < 0.5f);
                    }
                    let _e793 = phi_3021_;
                    if _e793 {
                        let _e796 = global.member_11[0u];
                        phi_19312_ = _e125;
                        phi_19311_ = 0i;
                        loop {
                            let _e800 = phi_19312_;
                            let _e802 = phi_19311_;
                            local_115 = _e800;
                            if (_e802 < i32(min(_e796, 16f))) {
                                continue;
                            } else {
                                break;
                            }
                            continuing {
                                let _e804 = (_e802 * 2i);
                                let _e807 = global.member_12[_e804];
                                let _e811 = global.member_12[(_e804 + 1i)];
                                let _e812 = _e807.xy;
                                let _e818 = max(_e807.w, 0.001f);
                                let _e819 = (6.2831855f / _e818);
                                let _e828 = global.member_2[0u];
                                phi_19312_ = (_e800 + (_e807.z * sin(((_e819 * dot((_e812 / vec2(max(length(_e812), 0.0001f))), _e784)) + (_e828 * (_e819 * (sqrt((1.56131f * _e818)) * _e811.y)))))));
                                phi_19311_ = (_e802 + 1i);
                            }
                        }
                        let _e6528 = local_115;
                        phi_19313_ = _e6528;
                        break;
                    }
                    let _e839 = global.member_7[1u];
                    let _e841 = i32(min(_e839, 16f));
                    phi_18828_ = 0f;
                    phi_18827_ = 0i;
                    loop {
                        let _e843 = phi_18828_;
                        let _e845 = phi_18827_;
                        local_11 = _e843;
                        local_113 = _e843;
                        local_114 = _e843;
                        if (_e845 < _e841) {
                            let _e849 = f32((1i << bitcast<u32>(_e845)));
                            let _e854 = (1f - smoothstep((_e849 * 15f), (_e849 * 30f), _e164));
                            if (_e854 <= 0f) {
                                phi_31948_ = _e843;
                                continue;
                            }
                            let _e857 = (_e784 / vec2((0.5f * _e849)));
                            let _e861 = vec3<f32>(_e857.x, _e857.y, f32(_e845));
                            let _e867 = textureSampleLevel(global_22, global_21, vec2<f32>(_e861.x, _e861.y), i32(_e861.z), 0f);
                            phi_31948_ = (_e843 + (_e867.y * _e854));
                            continue;
                        } else {
                            break;
                        }
                        continuing {
                            let _e872 = phi_31948_;
                            phi_18828_ = _e872;
                            phi_18827_ = (_e845 + 1i);
                        }
                    }
                    let _e876 = global.member_8[0u];
                    let _e879 = u32((max(_e876, 0f) + 0.5f));
                    let _e6525 = local_114;
                    phi_19132_ = _e6525;
                    if (_e879 != 0u) {
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e882 = (_e879 == 0u);
                                local_16 = _e882;
                                if _e882 {
                                    phi_18836_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e885 = global.member_10[0u];
                                let _e888 = min(i32((_e885 + 0.5f)), 7i);
                                if (_e888 <= 0i) {
                                    phi_18836_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e890 = textureDimensions(global_2, 0i);
                                let _e893 = f32(vec2<i32>(_e890).x);
                                phi_18829_ = 0i;
                                loop {
                                    let _e895 = phi_18829_;
                                    phi_18837_ = vec4<f32>();
                                    phi_18832_ = false;
                                    if (_e895 < 7i) {
                                        if (_e895 >= _e888) {
                                            phi_18837_ = vec4<f32>();
                                            phi_18832_ = false;
                                            break;
                                        }
                                        let _e900 = global.member_9[_e895];
                                        let _e906 = ((_e784 - _e900.xy) / vec2((_e900.z * _e893)));
                                        let _e908 = all((_e906 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_3407_ = _e908;
                                        if _e908 {
                                            phi_3407_ = all((_e906 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e912 = phi_3407_;
                                        if _e912 {
                                            let _e916 = vec3<f32>(_e906.x, _e906.y, f32(_e895));
                                            let _e922 = textureSampleLevel(global_2, global_1, vec2<f32>(_e916.x, _e916.y), i32(_e916.z), 0f);
                                            phi_18837_ = clamp(_e922, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_18832_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_18829_ = (_e895 + 1i);
                                    }
                                }
                                let _e926 = phi_18837_;
                                let _e928 = phi_18832_;
                                phi_18836_ = _e926;
                                if _e928 {
                                    break;
                                }
                                let _e929 = (_e888 - 1i);
                                let _e932 = global.member_9[_e929];
                                let _e939 = clamp(((_e784 - _e932.xy) / vec2((_e932.z * _e893))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e943 = vec3<f32>(_e939.x, _e939.y, f32(_e929));
                                let _e949 = textureSampleLevel(global_2, global_1, vec2<f32>(_e943.x, _e943.y), i32(_e943.z), 0f);
                                phi_18836_ = clamp(_e949, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e952 = phi_18836_;
                        let _e954 = ((_e879 & 1u) != 0u);
                        phi_3043_ = _e954;
                        if _e954 {
                            phi_3043_ = (_e952.x > 0.0001f);
                        }
                        let _e958 = phi_3043_;
                        let _e6474 = local_113;
                        phi_18960_ = _e6474;
                        if _e958 {
                            phi_18958_ = 0f;
                            phi_18957_ = 0i;
                            loop {
                                let _e960 = phi_18958_;
                                let _e962 = phi_18957_;
                                local_12 = _e960;
                                if (_e962 < _e841) {
                                    let _e966 = f32((1i << bitcast<u32>(_e962)));
                                    let _e971 = (1f - smoothstep((_e966 * 15f), (_e966 * 30f), _e164));
                                    if (_e971 <= 0f) {
                                        phi_31950_ = _e960;
                                        continue;
                                    }
                                    let _e974 = (_e784 / vec2((0.5f * _e966)));
                                    let _e978 = vec3<f32>(_e974.x, _e974.y, f32(_e962));
                                    let _e984 = textureSampleLevel(global_6, global_5, vec2<f32>(_e978.x, _e978.y), i32(_e978.z), 0f);
                                    phi_31950_ = (_e960 + (_e984.y * _e971));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e989 = phi_31950_;
                                    phi_18958_ = _e989;
                                    phi_18957_ = (_e962 + 1i);
                                }
                            }
                            let _e993 = local_11;
                            let _e995 = local_12;
                            phi_18960_ = mix(_e993, _e995, _e952.x);
                        }
                        let _e998 = phi_18960_;
                        let _e1000 = ((_e879 & 2u) != 0u);
                        phi_3061_ = _e1000;
                        if _e1000 {
                            phi_3061_ = (_e952.y > 0.0001f);
                        }
                        let _e1004 = phi_3061_;
                        phi_18964_ = _e998;
                        if _e1004 {
                            phi_18962_ = 0f;
                            phi_18961_ = 0i;
                            loop {
                                let _e1006 = phi_18962_;
                                let _e1008 = phi_18961_;
                                local_13 = _e1006;
                                if (_e1008 < _e841) {
                                    let _e1012 = f32((1i << bitcast<u32>(_e1008)));
                                    let _e1017 = (1f - smoothstep((_e1012 * 15f), (_e1012 * 30f), _e164));
                                    if (_e1017 <= 0f) {
                                        phi_31951_ = _e1006;
                                        continue;
                                    }
                                    let _e1020 = (_e784 / vec2((0.5f * _e1012)));
                                    let _e1024 = vec3<f32>(_e1020.x, _e1020.y, f32(_e1008));
                                    let _e1030 = textureSampleLevel(global_8, global_7, vec2<f32>(_e1024.x, _e1024.y), i32(_e1024.z), 0f);
                                    phi_31951_ = (_e1006 + (_e1030.y * _e1017));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1035 = phi_31951_;
                                    phi_18962_ = _e1035;
                                    phi_18961_ = (_e1008 + 1i);
                                }
                            }
                            let _e1039 = local_13;
                            phi_18964_ = mix(_e998, _e1039, _e952.y);
                        }
                        let _e1042 = phi_18964_;
                        let _e1044 = ((_e879 & 4u) != 0u);
                        phi_3079_ = _e1044;
                        if _e1044 {
                            phi_3079_ = (_e952.z > 0.0001f);
                        }
                        let _e1048 = phi_3079_;
                        phi_18968_ = _e1042;
                        if _e1048 {
                            phi_18966_ = 0f;
                            phi_18965_ = 0i;
                            loop {
                                let _e1050 = phi_18966_;
                                let _e1052 = phi_18965_;
                                local_14 = _e1050;
                                if (_e1052 < _e841) {
                                    let _e1056 = f32((1i << bitcast<u32>(_e1052)));
                                    let _e1061 = (1f - smoothstep((_e1056 * 15f), (_e1056 * 30f), _e164));
                                    if (_e1061 <= 0f) {
                                        phi_31952_ = _e1050;
                                        continue;
                                    }
                                    let _e1064 = (_e784 / vec2((0.5f * _e1056)));
                                    let _e1068 = vec3<f32>(_e1064.x, _e1064.y, f32(_e1052));
                                    let _e1074 = textureSampleLevel(global_10, global_9, vec2<f32>(_e1068.x, _e1068.y), i32(_e1068.z), 0f);
                                    phi_31952_ = (_e1050 + (_e1074.y * _e1061));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1079 = phi_31952_;
                                    phi_18966_ = _e1079;
                                    phi_18965_ = (_e1052 + 1i);
                                }
                            }
                            let _e1083 = local_14;
                            phi_18968_ = mix(_e1042, _e1083, _e952.z);
                        }
                        let _e1086 = phi_18968_;
                        let _e1088 = ((_e879 & 8u) != 0u);
                        phi_3097_ = _e1088;
                        if _e1088 {
                            phi_3097_ = (_e952.w > 0.0001f);
                        }
                        let _e1092 = phi_3097_;
                        phi_19112_ = _e1086;
                        if _e1092 {
                            phi_18970_ = 0f;
                            phi_18969_ = 0i;
                            loop {
                                let _e1094 = phi_18970_;
                                let _e1096 = phi_18969_;
                                local_15 = _e1094;
                                if (_e1096 < _e841) {
                                    let _e1100 = f32((1i << bitcast<u32>(_e1096)));
                                    let _e1105 = (1f - smoothstep((_e1100 * 15f), (_e1100 * 30f), _e164));
                                    if (_e1105 <= 0f) {
                                        phi_31953_ = _e1094;
                                        continue;
                                    }
                                    let _e1108 = (_e784 / vec2((0.5f * _e1100)));
                                    let _e1112 = vec3<f32>(_e1108.x, _e1108.y, f32(_e1096));
                                    let _e1118 = textureSampleLevel(global_12, global_11, vec2<f32>(_e1112.x, _e1112.y), i32(_e1112.z), 0f);
                                    phi_31953_ = (_e1094 + (_e1118.y * _e1105));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1123 = phi_31953_;
                                    phi_18970_ = _e1123;
                                    phi_18969_ = (_e1096 + 1i);
                                }
                            }
                            let _e1127 = local_15;
                            phi_19112_ = mix(_e1086, _e1127, _e952.w);
                        }
                        let _e1130 = phi_19112_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e1133 = local_16;
                                if _e1133 {
                                    phi_18978_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e1136 = global.member_10[0u];
                                let _e1139 = min(i32((_e1136 + 0.5f)), 7i);
                                if (_e1139 <= 0i) {
                                    phi_18978_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e1141 = textureDimensions(global_2, 0i);
                                let _e1144 = f32(vec2<i32>(_e1141).x);
                                phi_18971_ = 0i;
                                loop {
                                    let _e1146 = phi_18971_;
                                    phi_18979_ = vec4<f32>();
                                    phi_18974_ = false;
                                    if (_e1146 < 7i) {
                                        if (_e1146 >= _e1139) {
                                            phi_18979_ = vec4<f32>();
                                            phi_18974_ = false;
                                            break;
                                        }
                                        let _e1151 = global.member_9[_e1146];
                                        let _e1157 = ((_e784 - _e1151.xy) / vec2((_e1151.z * _e1144)));
                                        let _e1159 = all((_e1157 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_4074_ = _e1159;
                                        if _e1159 {
                                            phi_4074_ = all((_e1157 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e1163 = phi_4074_;
                                        if _e1163 {
                                            let _e1167 = vec3<f32>(_e1157.x, _e1157.y, f32(_e1146));
                                            let _e1173 = textureSampleLevel(global_4, global_3, vec2<f32>(_e1167.x, _e1167.y), i32(_e1167.z), 0f);
                                            phi_18979_ = clamp(_e1173, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_18974_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_18971_ = (_e1146 + 1i);
                                    }
                                }
                                let _e1177 = phi_18979_;
                                let _e1179 = phi_18974_;
                                phi_18978_ = _e1177;
                                if _e1179 {
                                    break;
                                }
                                let _e1180 = (_e1139 - 1i);
                                let _e1183 = global.member_9[_e1180];
                                let _e1190 = clamp(((_e784 - _e1183.xy) / vec2((_e1183.z * _e1144))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e1194 = vec3<f32>(_e1190.x, _e1190.y, f32(_e1180));
                                let _e1200 = textureSampleLevel(global_4, global_3, vec2<f32>(_e1194.x, _e1194.y), i32(_e1194.z), 0f);
                                phi_18978_ = clamp(_e1200, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e1203 = phi_18978_;
                        let _e1205 = ((_e879 & 16u) != 0u);
                        phi_3117_ = _e1205;
                        if _e1205 {
                            phi_3117_ = (_e1203.x > 0.0001f);
                        }
                        let _e1209 = phi_3117_;
                        phi_19121_ = _e1130;
                        if _e1209 {
                            phi_19119_ = 0f;
                            phi_19118_ = 0i;
                            loop {
                                let _e1211 = phi_19119_;
                                let _e1213 = phi_19118_;
                                local_17 = _e1211;
                                if (_e1213 < _e841) {
                                    let _e1217 = f32((1i << bitcast<u32>(_e1213)));
                                    let _e1222 = (1f - smoothstep((_e1217 * 15f), (_e1217 * 30f), _e164));
                                    if (_e1222 <= 0f) {
                                        phi_31987_ = _e1211;
                                        continue;
                                    }
                                    let _e1225 = (_e784 / vec2((0.5f * _e1217)));
                                    let _e1229 = vec3<f32>(_e1225.x, _e1225.y, f32(_e1213));
                                    let _e1235 = textureSampleLevel(global_14, global_13, vec2<f32>(_e1229.x, _e1229.y), i32(_e1229.z), 0f);
                                    phi_31987_ = (_e1211 + (_e1235.y * _e1222));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1240 = phi_31987_;
                                    phi_19119_ = _e1240;
                                    phi_19118_ = (_e1213 + 1i);
                                }
                            }
                            let _e1244 = local_17;
                            phi_19121_ = mix(_e1130, _e1244, _e1203.x);
                        }
                        let _e1247 = phi_19121_;
                        let _e1249 = ((_e879 & 32u) != 0u);
                        phi_3135_ = _e1249;
                        if _e1249 {
                            phi_3135_ = (_e1203.y > 0.0001f);
                        }
                        let _e1253 = phi_3135_;
                        phi_19125_ = _e1247;
                        if _e1253 {
                            phi_19123_ = 0f;
                            phi_19122_ = 0i;
                            loop {
                                let _e1255 = phi_19123_;
                                let _e1257 = phi_19122_;
                                local_18 = _e1255;
                                if (_e1257 < _e841) {
                                    let _e1261 = f32((1i << bitcast<u32>(_e1257)));
                                    let _e1266 = (1f - smoothstep((_e1261 * 15f), (_e1261 * 30f), _e164));
                                    if (_e1266 <= 0f) {
                                        phi_31988_ = _e1255;
                                        continue;
                                    }
                                    let _e1269 = (_e784 / vec2((0.5f * _e1261)));
                                    let _e1273 = vec3<f32>(_e1269.x, _e1269.y, f32(_e1257));
                                    let _e1279 = textureSampleLevel(global_16, global_15, vec2<f32>(_e1273.x, _e1273.y), i32(_e1273.z), 0f);
                                    phi_31988_ = (_e1255 + (_e1279.y * _e1266));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1284 = phi_31988_;
                                    phi_19123_ = _e1284;
                                    phi_19122_ = (_e1257 + 1i);
                                }
                            }
                            let _e1288 = local_18;
                            phi_19125_ = mix(_e1247, _e1288, _e1203.y);
                        }
                        let _e1291 = phi_19125_;
                        let _e1293 = ((_e879 & 64u) != 0u);
                        phi_3153_ = _e1293;
                        if _e1293 {
                            phi_3153_ = (_e1203.z > 0.0001f);
                        }
                        let _e1297 = phi_3153_;
                        phi_19129_ = _e1291;
                        if _e1297 {
                            phi_19127_ = 0f;
                            phi_19126_ = 0i;
                            loop {
                                let _e1299 = phi_19127_;
                                let _e1301 = phi_19126_;
                                local_19 = _e1299;
                                if (_e1301 < _e841) {
                                    let _e1305 = f32((1i << bitcast<u32>(_e1301)));
                                    let _e1310 = (1f - smoothstep((_e1305 * 15f), (_e1305 * 30f), _e164));
                                    if (_e1310 <= 0f) {
                                        phi_31989_ = _e1299;
                                        continue;
                                    }
                                    let _e1313 = (_e784 / vec2((0.5f * _e1305)));
                                    let _e1317 = vec3<f32>(_e1313.x, _e1313.y, f32(_e1301));
                                    let _e1323 = textureSampleLevel(global_18, global_17, vec2<f32>(_e1317.x, _e1317.y), i32(_e1317.z), 0f);
                                    phi_31989_ = (_e1299 + (_e1323.y * _e1310));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1328 = phi_31989_;
                                    phi_19127_ = _e1328;
                                    phi_19126_ = (_e1301 + 1i);
                                }
                            }
                            let _e1332 = local_19;
                            phi_19129_ = mix(_e1291, _e1332, _e1203.z);
                        }
                        let _e1335 = phi_19129_;
                        let _e1337 = ((_e879 & 128u) != 0u);
                        phi_3171_ = _e1337;
                        if _e1337 {
                            phi_3171_ = (_e1203.w > 0.0001f);
                        }
                        let _e1341 = phi_3171_;
                        phi_19133_ = _e1335;
                        if _e1341 {
                            phi_19131_ = 0f;
                            phi_19130_ = 0i;
                            loop {
                                let _e1343 = phi_19131_;
                                let _e1345 = phi_19130_;
                                local_20 = _e1343;
                                if (_e1345 < _e841) {
                                    let _e1349 = f32((1i << bitcast<u32>(_e1345)));
                                    let _e1354 = (1f - smoothstep((_e1349 * 15f), (_e1349 * 30f), _e164));
                                    if (_e1354 <= 0f) {
                                        phi_31990_ = _e1343;
                                        continue;
                                    }
                                    let _e1357 = (_e784 / vec2((0.5f * _e1349)));
                                    let _e1361 = vec3<f32>(_e1357.x, _e1357.y, f32(_e1345));
                                    let _e1367 = textureSampleLevel(global_20, global_19, vec2<f32>(_e1361.x, _e1361.y), i32(_e1361.z), 0f);
                                    phi_31990_ = (_e1343 + (_e1367.y * _e1354));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1372 = phi_31990_;
                                    phi_19131_ = _e1372;
                                    phi_19130_ = (_e1345 + 1i);
                                }
                            }
                            let _e1376 = local_20;
                            phi_19133_ = mix(_e1335, _e1376, _e1203.w);
                        }
                        let _e1379 = phi_19133_;
                        phi_19132_ = _e1379;
                    }
                    let _e1381 = phi_19132_;
                    let _e1384 = global.member_7[2u];
                    let _e1388 = global.member_7[3u];
                    phi_19313_ = (_e125 + clamp((_e1381 * _e1384), -(_e1388), _e1388));
                    break;
                }
            }
            let _e1393 = phi_19313_;
            let _e1394 = (_e169 + vec2<f32>(0.35f, 0f));
            switch bitcast<i32>(0u) {
                default: {
                    let _e1397 = local_21;
                    let _e6532 = local_116;
                    phi_4720_ = _e6532;
                    if _e1397 {
                        let _e1400 = global.member_7[1u];
                        phi_4720_ = (_e1400 < 0.5f);
                    }
                    let _e1403 = phi_4720_;
                    if _e1403 {
                        let _e1406 = global.member_11[0u];
                        phi_20011_ = _e125;
                        phi_20010_ = 0i;
                        loop {
                            let _e1410 = phi_20011_;
                            let _e1412 = phi_20010_;
                            local_119 = _e1410;
                            if (_e1412 < i32(min(_e1406, 16f))) {
                                continue;
                            } else {
                                break;
                            }
                            continuing {
                                let _e1414 = (_e1412 * 2i);
                                let _e1417 = global.member_12[_e1414];
                                let _e1421 = global.member_12[(_e1414 + 1i)];
                                let _e1422 = _e1417.xy;
                                let _e1428 = max(_e1417.w, 0.001f);
                                let _e1429 = (6.2831855f / _e1428);
                                let _e1438 = global.member_2[0u];
                                phi_20011_ = (_e1410 + (_e1417.z * sin(((_e1429 * dot((_e1422 / vec2(max(length(_e1422), 0.0001f))), _e1394)) + (_e1438 * (_e1429 * (sqrt((1.56131f * _e1428)) * _e1421.y)))))));
                                phi_20010_ = (_e1412 + 1i);
                            }
                        }
                        let _e6605 = local_119;
                        phi_20012_ = _e6605;
                        break;
                    }
                    let _e1449 = global.member_7[1u];
                    let _e1451 = i32(min(_e1449, 16f));
                    phi_19315_ = 0f;
                    phi_19314_ = 0i;
                    loop {
                        let _e1453 = phi_19315_;
                        let _e1455 = phi_19314_;
                        local_22 = _e1453;
                        local_117 = _e1453;
                        local_118 = _e1453;
                        if (_e1455 < _e1451) {
                            let _e1459 = f32((1i << bitcast<u32>(_e1455)));
                            let _e1464 = (1f - smoothstep((_e1459 * 15f), (_e1459 * 30f), _e164));
                            if (_e1464 <= 0f) {
                                phi_31991_ = _e1453;
                                continue;
                            }
                            let _e1467 = (_e1394 / vec2((0.5f * _e1459)));
                            let _e1471 = vec3<f32>(_e1467.x, _e1467.y, f32(_e1455));
                            let _e1477 = textureSampleLevel(global_22, global_21, vec2<f32>(_e1471.x, _e1471.y), i32(_e1471.z), 0f);
                            phi_31991_ = (_e1453 + (_e1477.y * _e1464));
                            continue;
                        } else {
                            break;
                        }
                        continuing {
                            let _e1482 = phi_31991_;
                            phi_19315_ = _e1482;
                            phi_19314_ = (_e1455 + 1i);
                        }
                    }
                    let _e1486 = global.member_8[0u];
                    let _e1489 = u32((max(_e1486, 0f) + 0.5f));
                    let _e6602 = local_118;
                    phi_19831_ = _e6602;
                    if (_e1489 != 0u) {
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e1492 = (_e1489 == 0u);
                                local_27 = _e1492;
                                if _e1492 {
                                    phi_19323_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e1495 = global.member_10[0u];
                                let _e1498 = min(i32((_e1495 + 0.5f)), 7i);
                                if (_e1498 <= 0i) {
                                    phi_19323_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e1500 = textureDimensions(global_2, 0i);
                                let _e1503 = f32(vec2<i32>(_e1500).x);
                                phi_19316_ = 0i;
                                loop {
                                    let _e1505 = phi_19316_;
                                    phi_19324_ = vec4<f32>();
                                    phi_19319_ = false;
                                    if (_e1505 < 7i) {
                                        if (_e1505 >= _e1498) {
                                            phi_19324_ = vec4<f32>();
                                            phi_19319_ = false;
                                            break;
                                        }
                                        let _e1510 = global.member_9[_e1505];
                                        let _e1516 = ((_e1394 - _e1510.xy) / vec2((_e1510.z * _e1503)));
                                        let _e1518 = all((_e1516 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_5106_ = _e1518;
                                        if _e1518 {
                                            phi_5106_ = all((_e1516 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e1522 = phi_5106_;
                                        if _e1522 {
                                            let _e1526 = vec3<f32>(_e1516.x, _e1516.y, f32(_e1505));
                                            let _e1532 = textureSampleLevel(global_2, global_1, vec2<f32>(_e1526.x, _e1526.y), i32(_e1526.z), 0f);
                                            phi_19324_ = clamp(_e1532, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_19319_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_19316_ = (_e1505 + 1i);
                                    }
                                }
                                let _e1536 = phi_19324_;
                                let _e1538 = phi_19319_;
                                phi_19323_ = _e1536;
                                if _e1538 {
                                    break;
                                }
                                let _e1539 = (_e1498 - 1i);
                                let _e1542 = global.member_9[_e1539];
                                let _e1549 = clamp(((_e1394 - _e1542.xy) / vec2((_e1542.z * _e1503))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e1553 = vec3<f32>(_e1549.x, _e1549.y, f32(_e1539));
                                let _e1559 = textureSampleLevel(global_2, global_1, vec2<f32>(_e1553.x, _e1553.y), i32(_e1553.z), 0f);
                                phi_19323_ = clamp(_e1559, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e1562 = phi_19323_;
                        let _e1564 = ((_e1489 & 1u) != 0u);
                        phi_4742_ = _e1564;
                        if _e1564 {
                            phi_4742_ = (_e1562.x > 0.0001f);
                        }
                        let _e1568 = phi_4742_;
                        let _e6551 = local_117;
                        phi_19553_ = _e6551;
                        if _e1568 {
                            phi_19551_ = 0f;
                            phi_19550_ = 0i;
                            loop {
                                let _e1570 = phi_19551_;
                                let _e1572 = phi_19550_;
                                local_23 = _e1570;
                                if (_e1572 < _e1451) {
                                    let _e1576 = f32((1i << bitcast<u32>(_e1572)));
                                    let _e1581 = (1f - smoothstep((_e1576 * 15f), (_e1576 * 30f), _e164));
                                    if (_e1581 <= 0f) {
                                        phi_31993_ = _e1570;
                                        continue;
                                    }
                                    let _e1584 = (_e1394 / vec2((0.5f * _e1576)));
                                    let _e1588 = vec3<f32>(_e1584.x, _e1584.y, f32(_e1572));
                                    let _e1594 = textureSampleLevel(global_6, global_5, vec2<f32>(_e1588.x, _e1588.y), i32(_e1588.z), 0f);
                                    phi_31993_ = (_e1570 + (_e1594.y * _e1581));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1599 = phi_31993_;
                                    phi_19551_ = _e1599;
                                    phi_19550_ = (_e1572 + 1i);
                                }
                            }
                            let _e1603 = local_22;
                            let _e1605 = local_23;
                            phi_19553_ = mix(_e1603, _e1605, _e1562.x);
                        }
                        let _e1608 = phi_19553_;
                        let _e1610 = ((_e1489 & 2u) != 0u);
                        phi_4760_ = _e1610;
                        if _e1610 {
                            phi_4760_ = (_e1562.y > 0.0001f);
                        }
                        let _e1614 = phi_4760_;
                        phi_19557_ = _e1608;
                        if _e1614 {
                            phi_19555_ = 0f;
                            phi_19554_ = 0i;
                            loop {
                                let _e1616 = phi_19555_;
                                let _e1618 = phi_19554_;
                                local_24 = _e1616;
                                if (_e1618 < _e1451) {
                                    let _e1622 = f32((1i << bitcast<u32>(_e1618)));
                                    let _e1627 = (1f - smoothstep((_e1622 * 15f), (_e1622 * 30f), _e164));
                                    if (_e1627 <= 0f) {
                                        phi_31994_ = _e1616;
                                        continue;
                                    }
                                    let _e1630 = (_e1394 / vec2((0.5f * _e1622)));
                                    let _e1634 = vec3<f32>(_e1630.x, _e1630.y, f32(_e1618));
                                    let _e1640 = textureSampleLevel(global_8, global_7, vec2<f32>(_e1634.x, _e1634.y), i32(_e1634.z), 0f);
                                    phi_31994_ = (_e1616 + (_e1640.y * _e1627));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1645 = phi_31994_;
                                    phi_19555_ = _e1645;
                                    phi_19554_ = (_e1618 + 1i);
                                }
                            }
                            let _e1649 = local_24;
                            phi_19557_ = mix(_e1608, _e1649, _e1562.y);
                        }
                        let _e1652 = phi_19557_;
                        let _e1654 = ((_e1489 & 4u) != 0u);
                        phi_4778_ = _e1654;
                        if _e1654 {
                            phi_4778_ = (_e1562.z > 0.0001f);
                        }
                        let _e1658 = phi_4778_;
                        phi_19561_ = _e1652;
                        if _e1658 {
                            phi_19559_ = 0f;
                            phi_19558_ = 0i;
                            loop {
                                let _e1660 = phi_19559_;
                                let _e1662 = phi_19558_;
                                local_25 = _e1660;
                                if (_e1662 < _e1451) {
                                    let _e1666 = f32((1i << bitcast<u32>(_e1662)));
                                    let _e1671 = (1f - smoothstep((_e1666 * 15f), (_e1666 * 30f), _e164));
                                    if (_e1671 <= 0f) {
                                        phi_31995_ = _e1660;
                                        continue;
                                    }
                                    let _e1674 = (_e1394 / vec2((0.5f * _e1666)));
                                    let _e1678 = vec3<f32>(_e1674.x, _e1674.y, f32(_e1662));
                                    let _e1684 = textureSampleLevel(global_10, global_9, vec2<f32>(_e1678.x, _e1678.y), i32(_e1678.z), 0f);
                                    phi_31995_ = (_e1660 + (_e1684.y * _e1671));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1689 = phi_31995_;
                                    phi_19559_ = _e1689;
                                    phi_19558_ = (_e1662 + 1i);
                                }
                            }
                            let _e1693 = local_25;
                            phi_19561_ = mix(_e1652, _e1693, _e1562.z);
                        }
                        let _e1696 = phi_19561_;
                        let _e1698 = ((_e1489 & 8u) != 0u);
                        phi_4796_ = _e1698;
                        if _e1698 {
                            phi_4796_ = (_e1562.w > 0.0001f);
                        }
                        let _e1702 = phi_4796_;
                        phi_19811_ = _e1696;
                        if _e1702 {
                            phi_19563_ = 0f;
                            phi_19562_ = 0i;
                            loop {
                                let _e1704 = phi_19563_;
                                let _e1706 = phi_19562_;
                                local_26 = _e1704;
                                if (_e1706 < _e1451) {
                                    let _e1710 = f32((1i << bitcast<u32>(_e1706)));
                                    let _e1715 = (1f - smoothstep((_e1710 * 15f), (_e1710 * 30f), _e164));
                                    if (_e1715 <= 0f) {
                                        phi_31996_ = _e1704;
                                        continue;
                                    }
                                    let _e1718 = (_e1394 / vec2((0.5f * _e1710)));
                                    let _e1722 = vec3<f32>(_e1718.x, _e1718.y, f32(_e1706));
                                    let _e1728 = textureSampleLevel(global_12, global_11, vec2<f32>(_e1722.x, _e1722.y), i32(_e1722.z), 0f);
                                    phi_31996_ = (_e1704 + (_e1728.y * _e1715));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1733 = phi_31996_;
                                    phi_19563_ = _e1733;
                                    phi_19562_ = (_e1706 + 1i);
                                }
                            }
                            let _e1737 = local_26;
                            phi_19811_ = mix(_e1696, _e1737, _e1562.w);
                        }
                        let _e1740 = phi_19811_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e1743 = local_27;
                                if _e1743 {
                                    phi_19571_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e1746 = global.member_10[0u];
                                let _e1749 = min(i32((_e1746 + 0.5f)), 7i);
                                if (_e1749 <= 0i) {
                                    phi_19571_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e1751 = textureDimensions(global_2, 0i);
                                let _e1754 = f32(vec2<i32>(_e1751).x);
                                phi_19564_ = 0i;
                                loop {
                                    let _e1756 = phi_19564_;
                                    phi_19572_ = vec4<f32>();
                                    phi_19567_ = false;
                                    if (_e1756 < 7i) {
                                        if (_e1756 >= _e1749) {
                                            phi_19572_ = vec4<f32>();
                                            phi_19567_ = false;
                                            break;
                                        }
                                        let _e1761 = global.member_9[_e1756];
                                        let _e1767 = ((_e1394 - _e1761.xy) / vec2((_e1761.z * _e1754)));
                                        let _e1769 = all((_e1767 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_5773_ = _e1769;
                                        if _e1769 {
                                            phi_5773_ = all((_e1767 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e1773 = phi_5773_;
                                        if _e1773 {
                                            let _e1777 = vec3<f32>(_e1767.x, _e1767.y, f32(_e1756));
                                            let _e1783 = textureSampleLevel(global_4, global_3, vec2<f32>(_e1777.x, _e1777.y), i32(_e1777.z), 0f);
                                            phi_19572_ = clamp(_e1783, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_19567_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_19564_ = (_e1756 + 1i);
                                    }
                                }
                                let _e1787 = phi_19572_;
                                let _e1789 = phi_19567_;
                                phi_19571_ = _e1787;
                                if _e1789 {
                                    break;
                                }
                                let _e1790 = (_e1749 - 1i);
                                let _e1793 = global.member_9[_e1790];
                                let _e1800 = clamp(((_e1394 - _e1793.xy) / vec2((_e1793.z * _e1754))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e1804 = vec3<f32>(_e1800.x, _e1800.y, f32(_e1790));
                                let _e1810 = textureSampleLevel(global_4, global_3, vec2<f32>(_e1804.x, _e1804.y), i32(_e1804.z), 0f);
                                phi_19571_ = clamp(_e1810, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e1813 = phi_19571_;
                        let _e1815 = ((_e1489 & 16u) != 0u);
                        phi_4816_ = _e1815;
                        if _e1815 {
                            phi_4816_ = (_e1813.x > 0.0001f);
                        }
                        let _e1819 = phi_4816_;
                        phi_19820_ = _e1740;
                        if _e1819 {
                            phi_19818_ = 0f;
                            phi_19817_ = 0i;
                            loop {
                                let _e1821 = phi_19818_;
                                let _e1823 = phi_19817_;
                                local_28 = _e1821;
                                if (_e1823 < _e1451) {
                                    let _e1827 = f32((1i << bitcast<u32>(_e1823)));
                                    let _e1832 = (1f - smoothstep((_e1827 * 15f), (_e1827 * 30f), _e164));
                                    if (_e1832 <= 0f) {
                                        phi_32030_ = _e1821;
                                        continue;
                                    }
                                    let _e1835 = (_e1394 / vec2((0.5f * _e1827)));
                                    let _e1839 = vec3<f32>(_e1835.x, _e1835.y, f32(_e1823));
                                    let _e1845 = textureSampleLevel(global_14, global_13, vec2<f32>(_e1839.x, _e1839.y), i32(_e1839.z), 0f);
                                    phi_32030_ = (_e1821 + (_e1845.y * _e1832));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1850 = phi_32030_;
                                    phi_19818_ = _e1850;
                                    phi_19817_ = (_e1823 + 1i);
                                }
                            }
                            let _e1854 = local_28;
                            phi_19820_ = mix(_e1740, _e1854, _e1813.x);
                        }
                        let _e1857 = phi_19820_;
                        let _e1859 = ((_e1489 & 32u) != 0u);
                        phi_4834_ = _e1859;
                        if _e1859 {
                            phi_4834_ = (_e1813.y > 0.0001f);
                        }
                        let _e1863 = phi_4834_;
                        phi_19824_ = _e1857;
                        if _e1863 {
                            phi_19822_ = 0f;
                            phi_19821_ = 0i;
                            loop {
                                let _e1865 = phi_19822_;
                                let _e1867 = phi_19821_;
                                local_29 = _e1865;
                                if (_e1867 < _e1451) {
                                    let _e1871 = f32((1i << bitcast<u32>(_e1867)));
                                    let _e1876 = (1f - smoothstep((_e1871 * 15f), (_e1871 * 30f), _e164));
                                    if (_e1876 <= 0f) {
                                        phi_32031_ = _e1865;
                                        continue;
                                    }
                                    let _e1879 = (_e1394 / vec2((0.5f * _e1871)));
                                    let _e1883 = vec3<f32>(_e1879.x, _e1879.y, f32(_e1867));
                                    let _e1889 = textureSampleLevel(global_16, global_15, vec2<f32>(_e1883.x, _e1883.y), i32(_e1883.z), 0f);
                                    phi_32031_ = (_e1865 + (_e1889.y * _e1876));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1894 = phi_32031_;
                                    phi_19822_ = _e1894;
                                    phi_19821_ = (_e1867 + 1i);
                                }
                            }
                            let _e1898 = local_29;
                            phi_19824_ = mix(_e1857, _e1898, _e1813.y);
                        }
                        let _e1901 = phi_19824_;
                        let _e1903 = ((_e1489 & 64u) != 0u);
                        phi_4852_ = _e1903;
                        if _e1903 {
                            phi_4852_ = (_e1813.z > 0.0001f);
                        }
                        let _e1907 = phi_4852_;
                        phi_19828_ = _e1901;
                        if _e1907 {
                            phi_19826_ = 0f;
                            phi_19825_ = 0i;
                            loop {
                                let _e1909 = phi_19826_;
                                let _e1911 = phi_19825_;
                                local_30 = _e1909;
                                if (_e1911 < _e1451) {
                                    let _e1915 = f32((1i << bitcast<u32>(_e1911)));
                                    let _e1920 = (1f - smoothstep((_e1915 * 15f), (_e1915 * 30f), _e164));
                                    if (_e1920 <= 0f) {
                                        phi_32032_ = _e1909;
                                        continue;
                                    }
                                    let _e1923 = (_e1394 / vec2((0.5f * _e1915)));
                                    let _e1927 = vec3<f32>(_e1923.x, _e1923.y, f32(_e1911));
                                    let _e1933 = textureSampleLevel(global_18, global_17, vec2<f32>(_e1927.x, _e1927.y), i32(_e1927.z), 0f);
                                    phi_32032_ = (_e1909 + (_e1933.y * _e1920));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1938 = phi_32032_;
                                    phi_19826_ = _e1938;
                                    phi_19825_ = (_e1911 + 1i);
                                }
                            }
                            let _e1942 = local_30;
                            phi_19828_ = mix(_e1901, _e1942, _e1813.z);
                        }
                        let _e1945 = phi_19828_;
                        let _e1947 = ((_e1489 & 128u) != 0u);
                        phi_4870_ = _e1947;
                        if _e1947 {
                            phi_4870_ = (_e1813.w > 0.0001f);
                        }
                        let _e1951 = phi_4870_;
                        phi_19832_ = _e1945;
                        if _e1951 {
                            phi_19830_ = 0f;
                            phi_19829_ = 0i;
                            loop {
                                let _e1953 = phi_19830_;
                                let _e1955 = phi_19829_;
                                local_31 = _e1953;
                                if (_e1955 < _e1451) {
                                    let _e1959 = f32((1i << bitcast<u32>(_e1955)));
                                    let _e1964 = (1f - smoothstep((_e1959 * 15f), (_e1959 * 30f), _e164));
                                    if (_e1964 <= 0f) {
                                        phi_32033_ = _e1953;
                                        continue;
                                    }
                                    let _e1967 = (_e1394 / vec2((0.5f * _e1959)));
                                    let _e1971 = vec3<f32>(_e1967.x, _e1967.y, f32(_e1955));
                                    let _e1977 = textureSampleLevel(global_20, global_19, vec2<f32>(_e1971.x, _e1971.y), i32(_e1971.z), 0f);
                                    phi_32033_ = (_e1953 + (_e1977.y * _e1964));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e1982 = phi_32033_;
                                    phi_19830_ = _e1982;
                                    phi_19829_ = (_e1955 + 1i);
                                }
                            }
                            let _e1986 = local_31;
                            phi_19832_ = mix(_e1945, _e1986, _e1813.w);
                        }
                        let _e1989 = phi_19832_;
                        phi_19831_ = _e1989;
                    }
                    let _e1991 = phi_19831_;
                    let _e1994 = global.member_7[2u];
                    let _e1998 = global.member_7[3u];
                    phi_20012_ = (_e125 + clamp((_e1991 * _e1994), -(_e1998), _e1998));
                    break;
                }
            }
            let _e2003 = phi_20012_;
            let _e2004 = (_e169 - vec2<f32>(0f, 0.35f));
            switch bitcast<i32>(0u) {
                default: {
                    let _e2007 = local_32;
                    let _e6609 = local_120;
                    phi_6419_ = _e6609;
                    if _e2007 {
                        let _e2010 = global.member_7[1u];
                        phi_6419_ = (_e2010 < 0.5f);
                    }
                    let _e2013 = phi_6419_;
                    if _e2013 {
                        let _e2016 = global.member_11[0u];
                        phi_20922_ = _e125;
                        phi_20921_ = 0i;
                        loop {
                            let _e2020 = phi_20922_;
                            let _e2022 = phi_20921_;
                            local_123 = _e2020;
                            if (_e2022 < i32(min(_e2016, 16f))) {
                                continue;
                            } else {
                                break;
                            }
                            continuing {
                                let _e2024 = (_e2022 * 2i);
                                let _e2027 = global.member_12[_e2024];
                                let _e2031 = global.member_12[(_e2024 + 1i)];
                                let _e2032 = _e2027.xy;
                                let _e2038 = max(_e2027.w, 0.001f);
                                let _e2039 = (6.2831855f / _e2038);
                                let _e2048 = global.member_2[0u];
                                phi_20922_ = (_e2020 + (_e2027.z * sin(((_e2039 * dot((_e2032 / vec2(max(length(_e2032), 0.0001f))), _e2004)) + (_e2048 * (_e2039 * (sqrt((1.56131f * _e2038)) * _e2031.y)))))));
                                phi_20921_ = (_e2022 + 1i);
                            }
                        }
                        let _e6682 = local_123;
                        phi_20923_ = _e6682;
                        break;
                    }
                    let _e2059 = global.member_7[1u];
                    let _e2061 = i32(min(_e2059, 16f));
                    phi_20014_ = 0f;
                    phi_20013_ = 0i;
                    loop {
                        let _e2063 = phi_20014_;
                        let _e2065 = phi_20013_;
                        local_33 = _e2063;
                        local_121 = _e2063;
                        local_122 = _e2063;
                        if (_e2065 < _e2061) {
                            let _e2069 = f32((1i << bitcast<u32>(_e2065)));
                            let _e2074 = (1f - smoothstep((_e2069 * 15f), (_e2069 * 30f), _e164));
                            if (_e2074 <= 0f) {
                                phi_32034_ = _e2063;
                                continue;
                            }
                            let _e2077 = (_e2004 / vec2((0.5f * _e2069)));
                            let _e2081 = vec3<f32>(_e2077.x, _e2077.y, f32(_e2065));
                            let _e2087 = textureSampleLevel(global_22, global_21, vec2<f32>(_e2081.x, _e2081.y), i32(_e2081.z), 0f);
                            phi_32034_ = (_e2063 + (_e2087.y * _e2074));
                            continue;
                        } else {
                            break;
                        }
                        continuing {
                            let _e2092 = phi_32034_;
                            phi_20014_ = _e2092;
                            phi_20013_ = (_e2065 + 1i);
                        }
                    }
                    let _e2096 = global.member_8[0u];
                    let _e2099 = u32((max(_e2096, 0f) + 0.5f));
                    let _e6679 = local_122;
                    phi_20742_ = _e6679;
                    if (_e2099 != 0u) {
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e2102 = (_e2099 == 0u);
                                local_38 = _e2102;
                                if _e2102 {
                                    phi_20022_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e2105 = global.member_10[0u];
                                let _e2108 = min(i32((_e2105 + 0.5f)), 7i);
                                if (_e2108 <= 0i) {
                                    phi_20022_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e2110 = textureDimensions(global_2, 0i);
                                let _e2113 = f32(vec2<i32>(_e2110).x);
                                phi_20015_ = 0i;
                                loop {
                                    let _e2115 = phi_20015_;
                                    phi_20023_ = vec4<f32>();
                                    phi_20018_ = false;
                                    if (_e2115 < 7i) {
                                        if (_e2115 >= _e2108) {
                                            phi_20023_ = vec4<f32>();
                                            phi_20018_ = false;
                                            break;
                                        }
                                        let _e2120 = global.member_9[_e2115];
                                        let _e2126 = ((_e2004 - _e2120.xy) / vec2((_e2120.z * _e2113)));
                                        let _e2128 = all((_e2126 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_6805_ = _e2128;
                                        if _e2128 {
                                            phi_6805_ = all((_e2126 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e2132 = phi_6805_;
                                        if _e2132 {
                                            let _e2136 = vec3<f32>(_e2126.x, _e2126.y, f32(_e2115));
                                            let _e2142 = textureSampleLevel(global_2, global_1, vec2<f32>(_e2136.x, _e2136.y), i32(_e2136.z), 0f);
                                            phi_20023_ = clamp(_e2142, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_20018_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_20015_ = (_e2115 + 1i);
                                    }
                                }
                                let _e2146 = phi_20023_;
                                let _e2148 = phi_20018_;
                                phi_20022_ = _e2146;
                                if _e2148 {
                                    break;
                                }
                                let _e2149 = (_e2108 - 1i);
                                let _e2152 = global.member_9[_e2149];
                                let _e2159 = clamp(((_e2004 - _e2152.xy) / vec2((_e2152.z * _e2113))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e2163 = vec3<f32>(_e2159.x, _e2159.y, f32(_e2149));
                                let _e2169 = textureSampleLevel(global_2, global_1, vec2<f32>(_e2163.x, _e2163.y), i32(_e2163.z), 0f);
                                phi_20022_ = clamp(_e2169, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e2172 = phi_20022_;
                        let _e2174 = ((_e2099 & 1u) != 0u);
                        phi_6441_ = _e2174;
                        if _e2174 {
                            phi_6441_ = (_e2172.x > 0.0001f);
                        }
                        let _e2178 = phi_6441_;
                        let _e6628 = local_121;
                        phi_20358_ = _e6628;
                        if _e2178 {
                            phi_20356_ = 0f;
                            phi_20355_ = 0i;
                            loop {
                                let _e2180 = phi_20356_;
                                let _e2182 = phi_20355_;
                                local_34 = _e2180;
                                if (_e2182 < _e2061) {
                                    let _e2186 = f32((1i << bitcast<u32>(_e2182)));
                                    let _e2191 = (1f - smoothstep((_e2186 * 15f), (_e2186 * 30f), _e164));
                                    if (_e2191 <= 0f) {
                                        phi_32036_ = _e2180;
                                        continue;
                                    }
                                    let _e2194 = (_e2004 / vec2((0.5f * _e2186)));
                                    let _e2198 = vec3<f32>(_e2194.x, _e2194.y, f32(_e2182));
                                    let _e2204 = textureSampleLevel(global_6, global_5, vec2<f32>(_e2198.x, _e2198.y), i32(_e2198.z), 0f);
                                    phi_32036_ = (_e2180 + (_e2204.y * _e2191));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2209 = phi_32036_;
                                    phi_20356_ = _e2209;
                                    phi_20355_ = (_e2182 + 1i);
                                }
                            }
                            let _e2213 = local_33;
                            let _e2215 = local_34;
                            phi_20358_ = mix(_e2213, _e2215, _e2172.x);
                        }
                        let _e2218 = phi_20358_;
                        let _e2220 = ((_e2099 & 2u) != 0u);
                        phi_6459_ = _e2220;
                        if _e2220 {
                            phi_6459_ = (_e2172.y > 0.0001f);
                        }
                        let _e2224 = phi_6459_;
                        phi_20362_ = _e2218;
                        if _e2224 {
                            phi_20360_ = 0f;
                            phi_20359_ = 0i;
                            loop {
                                let _e2226 = phi_20360_;
                                let _e2228 = phi_20359_;
                                local_35 = _e2226;
                                if (_e2228 < _e2061) {
                                    let _e2232 = f32((1i << bitcast<u32>(_e2228)));
                                    let _e2237 = (1f - smoothstep((_e2232 * 15f), (_e2232 * 30f), _e164));
                                    if (_e2237 <= 0f) {
                                        phi_32037_ = _e2226;
                                        continue;
                                    }
                                    let _e2240 = (_e2004 / vec2((0.5f * _e2232)));
                                    let _e2244 = vec3<f32>(_e2240.x, _e2240.y, f32(_e2228));
                                    let _e2250 = textureSampleLevel(global_8, global_7, vec2<f32>(_e2244.x, _e2244.y), i32(_e2244.z), 0f);
                                    phi_32037_ = (_e2226 + (_e2250.y * _e2237));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2255 = phi_32037_;
                                    phi_20360_ = _e2255;
                                    phi_20359_ = (_e2228 + 1i);
                                }
                            }
                            let _e2259 = local_35;
                            phi_20362_ = mix(_e2218, _e2259, _e2172.y);
                        }
                        let _e2262 = phi_20362_;
                        let _e2264 = ((_e2099 & 4u) != 0u);
                        phi_6477_ = _e2264;
                        if _e2264 {
                            phi_6477_ = (_e2172.z > 0.0001f);
                        }
                        let _e2268 = phi_6477_;
                        phi_20366_ = _e2262;
                        if _e2268 {
                            phi_20364_ = 0f;
                            phi_20363_ = 0i;
                            loop {
                                let _e2270 = phi_20364_;
                                let _e2272 = phi_20363_;
                                local_36 = _e2270;
                                if (_e2272 < _e2061) {
                                    let _e2276 = f32((1i << bitcast<u32>(_e2272)));
                                    let _e2281 = (1f - smoothstep((_e2276 * 15f), (_e2276 * 30f), _e164));
                                    if (_e2281 <= 0f) {
                                        phi_32038_ = _e2270;
                                        continue;
                                    }
                                    let _e2284 = (_e2004 / vec2((0.5f * _e2276)));
                                    let _e2288 = vec3<f32>(_e2284.x, _e2284.y, f32(_e2272));
                                    let _e2294 = textureSampleLevel(global_10, global_9, vec2<f32>(_e2288.x, _e2288.y), i32(_e2288.z), 0f);
                                    phi_32038_ = (_e2270 + (_e2294.y * _e2281));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2299 = phi_32038_;
                                    phi_20364_ = _e2299;
                                    phi_20363_ = (_e2272 + 1i);
                                }
                            }
                            let _e2303 = local_36;
                            phi_20366_ = mix(_e2262, _e2303, _e2172.z);
                        }
                        let _e2306 = phi_20366_;
                        let _e2308 = ((_e2099 & 8u) != 0u);
                        phi_6495_ = _e2308;
                        if _e2308 {
                            phi_6495_ = (_e2172.w > 0.0001f);
                        }
                        let _e2312 = phi_6495_;
                        phi_20722_ = _e2306;
                        if _e2312 {
                            phi_20368_ = 0f;
                            phi_20367_ = 0i;
                            loop {
                                let _e2314 = phi_20368_;
                                let _e2316 = phi_20367_;
                                local_37 = _e2314;
                                if (_e2316 < _e2061) {
                                    let _e2320 = f32((1i << bitcast<u32>(_e2316)));
                                    let _e2325 = (1f - smoothstep((_e2320 * 15f), (_e2320 * 30f), _e164));
                                    if (_e2325 <= 0f) {
                                        phi_32039_ = _e2314;
                                        continue;
                                    }
                                    let _e2328 = (_e2004 / vec2((0.5f * _e2320)));
                                    let _e2332 = vec3<f32>(_e2328.x, _e2328.y, f32(_e2316));
                                    let _e2338 = textureSampleLevel(global_12, global_11, vec2<f32>(_e2332.x, _e2332.y), i32(_e2332.z), 0f);
                                    phi_32039_ = (_e2314 + (_e2338.y * _e2325));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2343 = phi_32039_;
                                    phi_20368_ = _e2343;
                                    phi_20367_ = (_e2316 + 1i);
                                }
                            }
                            let _e2347 = local_37;
                            phi_20722_ = mix(_e2306, _e2347, _e2172.w);
                        }
                        let _e2350 = phi_20722_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e2353 = local_38;
                                if _e2353 {
                                    phi_20376_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e2356 = global.member_10[0u];
                                let _e2359 = min(i32((_e2356 + 0.5f)), 7i);
                                if (_e2359 <= 0i) {
                                    phi_20376_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e2361 = textureDimensions(global_2, 0i);
                                let _e2364 = f32(vec2<i32>(_e2361).x);
                                phi_20369_ = 0i;
                                loop {
                                    let _e2366 = phi_20369_;
                                    phi_20377_ = vec4<f32>();
                                    phi_20372_ = false;
                                    if (_e2366 < 7i) {
                                        if (_e2366 >= _e2359) {
                                            phi_20377_ = vec4<f32>();
                                            phi_20372_ = false;
                                            break;
                                        }
                                        let _e2371 = global.member_9[_e2366];
                                        let _e2377 = ((_e2004 - _e2371.xy) / vec2((_e2371.z * _e2364)));
                                        let _e2379 = all((_e2377 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_7472_ = _e2379;
                                        if _e2379 {
                                            phi_7472_ = all((_e2377 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e2383 = phi_7472_;
                                        if _e2383 {
                                            let _e2387 = vec3<f32>(_e2377.x, _e2377.y, f32(_e2366));
                                            let _e2393 = textureSampleLevel(global_4, global_3, vec2<f32>(_e2387.x, _e2387.y), i32(_e2387.z), 0f);
                                            phi_20377_ = clamp(_e2393, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_20372_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_20369_ = (_e2366 + 1i);
                                    }
                                }
                                let _e2397 = phi_20377_;
                                let _e2399 = phi_20372_;
                                phi_20376_ = _e2397;
                                if _e2399 {
                                    break;
                                }
                                let _e2400 = (_e2359 - 1i);
                                let _e2403 = global.member_9[_e2400];
                                let _e2410 = clamp(((_e2004 - _e2403.xy) / vec2((_e2403.z * _e2364))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e2414 = vec3<f32>(_e2410.x, _e2410.y, f32(_e2400));
                                let _e2420 = textureSampleLevel(global_4, global_3, vec2<f32>(_e2414.x, _e2414.y), i32(_e2414.z), 0f);
                                phi_20376_ = clamp(_e2420, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e2423 = phi_20376_;
                        let _e2425 = ((_e2099 & 16u) != 0u);
                        phi_6515_ = _e2425;
                        if _e2425 {
                            phi_6515_ = (_e2423.x > 0.0001f);
                        }
                        let _e2429 = phi_6515_;
                        phi_20731_ = _e2350;
                        if _e2429 {
                            phi_20729_ = 0f;
                            phi_20728_ = 0i;
                            loop {
                                let _e2431 = phi_20729_;
                                let _e2433 = phi_20728_;
                                local_39 = _e2431;
                                if (_e2433 < _e2061) {
                                    let _e2437 = f32((1i << bitcast<u32>(_e2433)));
                                    let _e2442 = (1f - smoothstep((_e2437 * 15f), (_e2437 * 30f), _e164));
                                    if (_e2442 <= 0f) {
                                        phi_32073_ = _e2431;
                                        continue;
                                    }
                                    let _e2445 = (_e2004 / vec2((0.5f * _e2437)));
                                    let _e2449 = vec3<f32>(_e2445.x, _e2445.y, f32(_e2433));
                                    let _e2455 = textureSampleLevel(global_14, global_13, vec2<f32>(_e2449.x, _e2449.y), i32(_e2449.z), 0f);
                                    phi_32073_ = (_e2431 + (_e2455.y * _e2442));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2460 = phi_32073_;
                                    phi_20729_ = _e2460;
                                    phi_20728_ = (_e2433 + 1i);
                                }
                            }
                            let _e2464 = local_39;
                            phi_20731_ = mix(_e2350, _e2464, _e2423.x);
                        }
                        let _e2467 = phi_20731_;
                        let _e2469 = ((_e2099 & 32u) != 0u);
                        phi_6533_ = _e2469;
                        if _e2469 {
                            phi_6533_ = (_e2423.y > 0.0001f);
                        }
                        let _e2473 = phi_6533_;
                        phi_20735_ = _e2467;
                        if _e2473 {
                            phi_20733_ = 0f;
                            phi_20732_ = 0i;
                            loop {
                                let _e2475 = phi_20733_;
                                let _e2477 = phi_20732_;
                                local_40 = _e2475;
                                if (_e2477 < _e2061) {
                                    let _e2481 = f32((1i << bitcast<u32>(_e2477)));
                                    let _e2486 = (1f - smoothstep((_e2481 * 15f), (_e2481 * 30f), _e164));
                                    if (_e2486 <= 0f) {
                                        phi_32074_ = _e2475;
                                        continue;
                                    }
                                    let _e2489 = (_e2004 / vec2((0.5f * _e2481)));
                                    let _e2493 = vec3<f32>(_e2489.x, _e2489.y, f32(_e2477));
                                    let _e2499 = textureSampleLevel(global_16, global_15, vec2<f32>(_e2493.x, _e2493.y), i32(_e2493.z), 0f);
                                    phi_32074_ = (_e2475 + (_e2499.y * _e2486));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2504 = phi_32074_;
                                    phi_20733_ = _e2504;
                                    phi_20732_ = (_e2477 + 1i);
                                }
                            }
                            let _e2508 = local_40;
                            phi_20735_ = mix(_e2467, _e2508, _e2423.y);
                        }
                        let _e2511 = phi_20735_;
                        let _e2513 = ((_e2099 & 64u) != 0u);
                        phi_6551_ = _e2513;
                        if _e2513 {
                            phi_6551_ = (_e2423.z > 0.0001f);
                        }
                        let _e2517 = phi_6551_;
                        phi_20739_ = _e2511;
                        if _e2517 {
                            phi_20737_ = 0f;
                            phi_20736_ = 0i;
                            loop {
                                let _e2519 = phi_20737_;
                                let _e2521 = phi_20736_;
                                local_41 = _e2519;
                                if (_e2521 < _e2061) {
                                    let _e2525 = f32((1i << bitcast<u32>(_e2521)));
                                    let _e2530 = (1f - smoothstep((_e2525 * 15f), (_e2525 * 30f), _e164));
                                    if (_e2530 <= 0f) {
                                        phi_32075_ = _e2519;
                                        continue;
                                    }
                                    let _e2533 = (_e2004 / vec2((0.5f * _e2525)));
                                    let _e2537 = vec3<f32>(_e2533.x, _e2533.y, f32(_e2521));
                                    let _e2543 = textureSampleLevel(global_18, global_17, vec2<f32>(_e2537.x, _e2537.y), i32(_e2537.z), 0f);
                                    phi_32075_ = (_e2519 + (_e2543.y * _e2530));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2548 = phi_32075_;
                                    phi_20737_ = _e2548;
                                    phi_20736_ = (_e2521 + 1i);
                                }
                            }
                            let _e2552 = local_41;
                            phi_20739_ = mix(_e2511, _e2552, _e2423.z);
                        }
                        let _e2555 = phi_20739_;
                        let _e2557 = ((_e2099 & 128u) != 0u);
                        phi_6569_ = _e2557;
                        if _e2557 {
                            phi_6569_ = (_e2423.w > 0.0001f);
                        }
                        let _e2561 = phi_6569_;
                        phi_20743_ = _e2555;
                        if _e2561 {
                            phi_20741_ = 0f;
                            phi_20740_ = 0i;
                            loop {
                                let _e2563 = phi_20741_;
                                let _e2565 = phi_20740_;
                                local_42 = _e2563;
                                if (_e2565 < _e2061) {
                                    let _e2569 = f32((1i << bitcast<u32>(_e2565)));
                                    let _e2574 = (1f - smoothstep((_e2569 * 15f), (_e2569 * 30f), _e164));
                                    if (_e2574 <= 0f) {
                                        phi_32076_ = _e2563;
                                        continue;
                                    }
                                    let _e2577 = (_e2004 / vec2((0.5f * _e2569)));
                                    let _e2581 = vec3<f32>(_e2577.x, _e2577.y, f32(_e2565));
                                    let _e2587 = textureSampleLevel(global_20, global_19, vec2<f32>(_e2581.x, _e2581.y), i32(_e2581.z), 0f);
                                    phi_32076_ = (_e2563 + (_e2587.y * _e2574));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2592 = phi_32076_;
                                    phi_20741_ = _e2592;
                                    phi_20740_ = (_e2565 + 1i);
                                }
                            }
                            let _e2596 = local_42;
                            phi_20743_ = mix(_e2555, _e2596, _e2423.w);
                        }
                        let _e2599 = phi_20743_;
                        phi_20742_ = _e2599;
                    }
                    let _e2601 = phi_20742_;
                    let _e2604 = global.member_7[2u];
                    let _e2608 = global.member_7[3u];
                    phi_20923_ = (_e125 + clamp((_e2601 * _e2604), -(_e2608), _e2608));
                    break;
                }
            }
            let _e2613 = phi_20923_;
            let _e2614 = (_e169 + vec2<f32>(0f, 0.35f));
            switch bitcast<i32>(0u) {
                default: {
                    let _e2617 = local_43;
                    let _e6686 = local_124;
                    phi_8118_ = _e6686;
                    if _e2617 {
                        let _e2620 = global.member_7[1u];
                        phi_8118_ = (_e2620 < 0.5f);
                    }
                    let _e2623 = phi_8118_;
                    if _e2623 {
                        let _e2626 = global.member_11[0u];
                        phi_22045_ = _e125;
                        phi_22044_ = 0i;
                        loop {
                            let _e2630 = phi_22045_;
                            let _e2632 = phi_22044_;
                            local_127 = _e2630;
                            if (_e2632 < i32(min(_e2626, 16f))) {
                                continue;
                            } else {
                                break;
                            }
                            continuing {
                                let _e2634 = (_e2632 * 2i);
                                let _e2637 = global.member_12[_e2634];
                                let _e2641 = global.member_12[(_e2634 + 1i)];
                                let _e2642 = _e2637.xy;
                                let _e2648 = max(_e2637.w, 0.001f);
                                let _e2649 = (6.2831855f / _e2648);
                                let _e2658 = global.member_2[0u];
                                phi_22045_ = (_e2630 + (_e2637.z * sin(((_e2649 * dot((_e2642 / vec2(max(length(_e2642), 0.0001f))), _e2614)) + (_e2658 * (_e2649 * (sqrt((1.56131f * _e2648)) * _e2641.y)))))));
                                phi_22044_ = (_e2632 + 1i);
                            }
                        }
                        let _e6759 = local_127;
                        phi_22046_ = _e6759;
                        break;
                    }
                    let _e2669 = global.member_7[1u];
                    let _e2671 = i32(min(_e2669, 16f));
                    phi_20925_ = 0f;
                    phi_20924_ = 0i;
                    loop {
                        let _e2673 = phi_20925_;
                        let _e2675 = phi_20924_;
                        local_44 = _e2673;
                        local_125 = _e2673;
                        local_126 = _e2673;
                        if (_e2675 < _e2671) {
                            let _e2679 = f32((1i << bitcast<u32>(_e2675)));
                            let _e2684 = (1f - smoothstep((_e2679 * 15f), (_e2679 * 30f), _e164));
                            if (_e2684 <= 0f) {
                                phi_32077_ = _e2673;
                                continue;
                            }
                            let _e2687 = (_e2614 / vec2((0.5f * _e2679)));
                            let _e2691 = vec3<f32>(_e2687.x, _e2687.y, f32(_e2675));
                            let _e2697 = textureSampleLevel(global_22, global_21, vec2<f32>(_e2691.x, _e2691.y), i32(_e2691.z), 0f);
                            phi_32077_ = (_e2673 + (_e2697.y * _e2684));
                            continue;
                        } else {
                            break;
                        }
                        continuing {
                            let _e2702 = phi_32077_;
                            phi_20925_ = _e2702;
                            phi_20924_ = (_e2675 + 1i);
                        }
                    }
                    let _e2706 = global.member_8[0u];
                    let _e2709 = u32((max(_e2706, 0f) + 0.5f));
                    let _e6756 = local_126;
                    phi_21865_ = _e6756;
                    if (_e2709 != 0u) {
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e2712 = (_e2709 == 0u);
                                local_49 = _e2712;
                                if _e2712 {
                                    phi_20933_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e2715 = global.member_10[0u];
                                let _e2718 = min(i32((_e2715 + 0.5f)), 7i);
                                if (_e2718 <= 0i) {
                                    phi_20933_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e2720 = textureDimensions(global_2, 0i);
                                let _e2723 = f32(vec2<i32>(_e2720).x);
                                phi_20926_ = 0i;
                                loop {
                                    let _e2725 = phi_20926_;
                                    phi_20934_ = vec4<f32>();
                                    phi_20929_ = false;
                                    if (_e2725 < 7i) {
                                        if (_e2725 >= _e2718) {
                                            phi_20934_ = vec4<f32>();
                                            phi_20929_ = false;
                                            break;
                                        }
                                        let _e2730 = global.member_9[_e2725];
                                        let _e2736 = ((_e2614 - _e2730.xy) / vec2((_e2730.z * _e2723)));
                                        let _e2738 = all((_e2736 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_8504_ = _e2738;
                                        if _e2738 {
                                            phi_8504_ = all((_e2736 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e2742 = phi_8504_;
                                        if _e2742 {
                                            let _e2746 = vec3<f32>(_e2736.x, _e2736.y, f32(_e2725));
                                            let _e2752 = textureSampleLevel(global_2, global_1, vec2<f32>(_e2746.x, _e2746.y), i32(_e2746.z), 0f);
                                            phi_20934_ = clamp(_e2752, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_20929_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_20926_ = (_e2725 + 1i);
                                    }
                                }
                                let _e2756 = phi_20934_;
                                let _e2758 = phi_20929_;
                                phi_20933_ = _e2756;
                                if _e2758 {
                                    break;
                                }
                                let _e2759 = (_e2718 - 1i);
                                let _e2762 = global.member_9[_e2759];
                                let _e2769 = clamp(((_e2614 - _e2762.xy) / vec2((_e2762.z * _e2723))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e2773 = vec3<f32>(_e2769.x, _e2769.y, f32(_e2759));
                                let _e2779 = textureSampleLevel(global_2, global_1, vec2<f32>(_e2773.x, _e2773.y), i32(_e2773.z), 0f);
                                phi_20933_ = clamp(_e2779, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e2782 = phi_20933_;
                        let _e2784 = ((_e2709 & 1u) != 0u);
                        phi_8140_ = _e2784;
                        if _e2784 {
                            phi_8140_ = (_e2782.x > 0.0001f);
                        }
                        let _e2788 = phi_8140_;
                        let _e6705 = local_125;
                        phi_21375_ = _e6705;
                        if _e2788 {
                            phi_21373_ = 0f;
                            phi_21372_ = 0i;
                            loop {
                                let _e2790 = phi_21373_;
                                let _e2792 = phi_21372_;
                                local_45 = _e2790;
                                if (_e2792 < _e2671) {
                                    let _e2796 = f32((1i << bitcast<u32>(_e2792)));
                                    let _e2801 = (1f - smoothstep((_e2796 * 15f), (_e2796 * 30f), _e164));
                                    if (_e2801 <= 0f) {
                                        phi_32079_ = _e2790;
                                        continue;
                                    }
                                    let _e2804 = (_e2614 / vec2((0.5f * _e2796)));
                                    let _e2808 = vec3<f32>(_e2804.x, _e2804.y, f32(_e2792));
                                    let _e2814 = textureSampleLevel(global_6, global_5, vec2<f32>(_e2808.x, _e2808.y), i32(_e2808.z), 0f);
                                    phi_32079_ = (_e2790 + (_e2814.y * _e2801));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2819 = phi_32079_;
                                    phi_21373_ = _e2819;
                                    phi_21372_ = (_e2792 + 1i);
                                }
                            }
                            let _e2823 = local_44;
                            let _e2825 = local_45;
                            phi_21375_ = mix(_e2823, _e2825, _e2782.x);
                        }
                        let _e2828 = phi_21375_;
                        let _e2830 = ((_e2709 & 2u) != 0u);
                        phi_8158_ = _e2830;
                        if _e2830 {
                            phi_8158_ = (_e2782.y > 0.0001f);
                        }
                        let _e2834 = phi_8158_;
                        phi_21379_ = _e2828;
                        if _e2834 {
                            phi_21377_ = 0f;
                            phi_21376_ = 0i;
                            loop {
                                let _e2836 = phi_21377_;
                                let _e2838 = phi_21376_;
                                local_46 = _e2836;
                                if (_e2838 < _e2671) {
                                    let _e2842 = f32((1i << bitcast<u32>(_e2838)));
                                    let _e2847 = (1f - smoothstep((_e2842 * 15f), (_e2842 * 30f), _e164));
                                    if (_e2847 <= 0f) {
                                        phi_32080_ = _e2836;
                                        continue;
                                    }
                                    let _e2850 = (_e2614 / vec2((0.5f * _e2842)));
                                    let _e2854 = vec3<f32>(_e2850.x, _e2850.y, f32(_e2838));
                                    let _e2860 = textureSampleLevel(global_8, global_7, vec2<f32>(_e2854.x, _e2854.y), i32(_e2854.z), 0f);
                                    phi_32080_ = (_e2836 + (_e2860.y * _e2847));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2865 = phi_32080_;
                                    phi_21377_ = _e2865;
                                    phi_21376_ = (_e2838 + 1i);
                                }
                            }
                            let _e2869 = local_46;
                            phi_21379_ = mix(_e2828, _e2869, _e2782.y);
                        }
                        let _e2872 = phi_21379_;
                        let _e2874 = ((_e2709 & 4u) != 0u);
                        phi_8176_ = _e2874;
                        if _e2874 {
                            phi_8176_ = (_e2782.z > 0.0001f);
                        }
                        let _e2878 = phi_8176_;
                        phi_21383_ = _e2872;
                        if _e2878 {
                            phi_21381_ = 0f;
                            phi_21380_ = 0i;
                            loop {
                                let _e2880 = phi_21381_;
                                let _e2882 = phi_21380_;
                                local_47 = _e2880;
                                if (_e2882 < _e2671) {
                                    let _e2886 = f32((1i << bitcast<u32>(_e2882)));
                                    let _e2891 = (1f - smoothstep((_e2886 * 15f), (_e2886 * 30f), _e164));
                                    if (_e2891 <= 0f) {
                                        phi_32081_ = _e2880;
                                        continue;
                                    }
                                    let _e2894 = (_e2614 / vec2((0.5f * _e2886)));
                                    let _e2898 = vec3<f32>(_e2894.x, _e2894.y, f32(_e2882));
                                    let _e2904 = textureSampleLevel(global_10, global_9, vec2<f32>(_e2898.x, _e2898.y), i32(_e2898.z), 0f);
                                    phi_32081_ = (_e2880 + (_e2904.y * _e2891));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2909 = phi_32081_;
                                    phi_21381_ = _e2909;
                                    phi_21380_ = (_e2882 + 1i);
                                }
                            }
                            let _e2913 = local_47;
                            phi_21383_ = mix(_e2872, _e2913, _e2782.z);
                        }
                        let _e2916 = phi_21383_;
                        let _e2918 = ((_e2709 & 8u) != 0u);
                        phi_8194_ = _e2918;
                        if _e2918 {
                            phi_8194_ = (_e2782.w > 0.0001f);
                        }
                        let _e2922 = phi_8194_;
                        phi_21845_ = _e2916;
                        if _e2922 {
                            phi_21385_ = 0f;
                            phi_21384_ = 0i;
                            loop {
                                let _e2924 = phi_21385_;
                                let _e2926 = phi_21384_;
                                local_48 = _e2924;
                                if (_e2926 < _e2671) {
                                    let _e2930 = f32((1i << bitcast<u32>(_e2926)));
                                    let _e2935 = (1f - smoothstep((_e2930 * 15f), (_e2930 * 30f), _e164));
                                    if (_e2935 <= 0f) {
                                        phi_32082_ = _e2924;
                                        continue;
                                    }
                                    let _e2938 = (_e2614 / vec2((0.5f * _e2930)));
                                    let _e2942 = vec3<f32>(_e2938.x, _e2938.y, f32(_e2926));
                                    let _e2948 = textureSampleLevel(global_12, global_11, vec2<f32>(_e2942.x, _e2942.y), i32(_e2942.z), 0f);
                                    phi_32082_ = (_e2924 + (_e2948.y * _e2935));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e2953 = phi_32082_;
                                    phi_21385_ = _e2953;
                                    phi_21384_ = (_e2926 + 1i);
                                }
                            }
                            let _e2957 = local_48;
                            phi_21845_ = mix(_e2916, _e2957, _e2782.w);
                        }
                        let _e2960 = phi_21845_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e2963 = local_49;
                                if _e2963 {
                                    phi_21393_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e2966 = global.member_10[0u];
                                let _e2969 = min(i32((_e2966 + 0.5f)), 7i);
                                if (_e2969 <= 0i) {
                                    phi_21393_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e2971 = textureDimensions(global_2, 0i);
                                let _e2974 = f32(vec2<i32>(_e2971).x);
                                phi_21386_ = 0i;
                                loop {
                                    let _e2976 = phi_21386_;
                                    phi_21394_ = vec4<f32>();
                                    phi_21389_ = false;
                                    if (_e2976 < 7i) {
                                        if (_e2976 >= _e2969) {
                                            phi_21394_ = vec4<f32>();
                                            phi_21389_ = false;
                                            break;
                                        }
                                        let _e2981 = global.member_9[_e2976];
                                        let _e2987 = ((_e2614 - _e2981.xy) / vec2((_e2981.z * _e2974)));
                                        let _e2989 = all((_e2987 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_9171_ = _e2989;
                                        if _e2989 {
                                            phi_9171_ = all((_e2987 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e2993 = phi_9171_;
                                        if _e2993 {
                                            let _e2997 = vec3<f32>(_e2987.x, _e2987.y, f32(_e2976));
                                            let _e3003 = textureSampleLevel(global_4, global_3, vec2<f32>(_e2997.x, _e2997.y), i32(_e2997.z), 0f);
                                            phi_21394_ = clamp(_e3003, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_21389_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_21386_ = (_e2976 + 1i);
                                    }
                                }
                                let _e3007 = phi_21394_;
                                let _e3009 = phi_21389_;
                                phi_21393_ = _e3007;
                                if _e3009 {
                                    break;
                                }
                                let _e3010 = (_e2969 - 1i);
                                let _e3013 = global.member_9[_e3010];
                                let _e3020 = clamp(((_e2614 - _e3013.xy) / vec2((_e3013.z * _e2974))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e3024 = vec3<f32>(_e3020.x, _e3020.y, f32(_e3010));
                                let _e3030 = textureSampleLevel(global_4, global_3, vec2<f32>(_e3024.x, _e3024.y), i32(_e3024.z), 0f);
                                phi_21393_ = clamp(_e3030, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e3033 = phi_21393_;
                        let _e3035 = ((_e2709 & 16u) != 0u);
                        phi_8214_ = _e3035;
                        if _e3035 {
                            phi_8214_ = (_e3033.x > 0.0001f);
                        }
                        let _e3039 = phi_8214_;
                        phi_21854_ = _e2960;
                        if _e3039 {
                            phi_21852_ = 0f;
                            phi_21851_ = 0i;
                            loop {
                                let _e3041 = phi_21852_;
                                let _e3043 = phi_21851_;
                                local_50 = _e3041;
                                if (_e3043 < _e2671) {
                                    let _e3047 = f32((1i << bitcast<u32>(_e3043)));
                                    let _e3052 = (1f - smoothstep((_e3047 * 15f), (_e3047 * 30f), _e164));
                                    if (_e3052 <= 0f) {
                                        phi_32116_ = _e3041;
                                        continue;
                                    }
                                    let _e3055 = (_e2614 / vec2((0.5f * _e3047)));
                                    let _e3059 = vec3<f32>(_e3055.x, _e3055.y, f32(_e3043));
                                    let _e3065 = textureSampleLevel(global_14, global_13, vec2<f32>(_e3059.x, _e3059.y), i32(_e3059.z), 0f);
                                    phi_32116_ = (_e3041 + (_e3065.y * _e3052));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e3070 = phi_32116_;
                                    phi_21852_ = _e3070;
                                    phi_21851_ = (_e3043 + 1i);
                                }
                            }
                            let _e3074 = local_50;
                            phi_21854_ = mix(_e2960, _e3074, _e3033.x);
                        }
                        let _e3077 = phi_21854_;
                        let _e3079 = ((_e2709 & 32u) != 0u);
                        phi_8232_ = _e3079;
                        if _e3079 {
                            phi_8232_ = (_e3033.y > 0.0001f);
                        }
                        let _e3083 = phi_8232_;
                        phi_21858_ = _e3077;
                        if _e3083 {
                            phi_21856_ = 0f;
                            phi_21855_ = 0i;
                            loop {
                                let _e3085 = phi_21856_;
                                let _e3087 = phi_21855_;
                                local_51 = _e3085;
                                if (_e3087 < _e2671) {
                                    let _e3091 = f32((1i << bitcast<u32>(_e3087)));
                                    let _e3096 = (1f - smoothstep((_e3091 * 15f), (_e3091 * 30f), _e164));
                                    if (_e3096 <= 0f) {
                                        phi_32117_ = _e3085;
                                        continue;
                                    }
                                    let _e3099 = (_e2614 / vec2((0.5f * _e3091)));
                                    let _e3103 = vec3<f32>(_e3099.x, _e3099.y, f32(_e3087));
                                    let _e3109 = textureSampleLevel(global_16, global_15, vec2<f32>(_e3103.x, _e3103.y), i32(_e3103.z), 0f);
                                    phi_32117_ = (_e3085 + (_e3109.y * _e3096));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e3114 = phi_32117_;
                                    phi_21856_ = _e3114;
                                    phi_21855_ = (_e3087 + 1i);
                                }
                            }
                            let _e3118 = local_51;
                            phi_21858_ = mix(_e3077, _e3118, _e3033.y);
                        }
                        let _e3121 = phi_21858_;
                        let _e3123 = ((_e2709 & 64u) != 0u);
                        phi_8250_ = _e3123;
                        if _e3123 {
                            phi_8250_ = (_e3033.z > 0.0001f);
                        }
                        let _e3127 = phi_8250_;
                        phi_21862_ = _e3121;
                        if _e3127 {
                            phi_21860_ = 0f;
                            phi_21859_ = 0i;
                            loop {
                                let _e3129 = phi_21860_;
                                let _e3131 = phi_21859_;
                                local_52 = _e3129;
                                if (_e3131 < _e2671) {
                                    let _e3135 = f32((1i << bitcast<u32>(_e3131)));
                                    let _e3140 = (1f - smoothstep((_e3135 * 15f), (_e3135 * 30f), _e164));
                                    if (_e3140 <= 0f) {
                                        phi_32118_ = _e3129;
                                        continue;
                                    }
                                    let _e3143 = (_e2614 / vec2((0.5f * _e3135)));
                                    let _e3147 = vec3<f32>(_e3143.x, _e3143.y, f32(_e3131));
                                    let _e3153 = textureSampleLevel(global_18, global_17, vec2<f32>(_e3147.x, _e3147.y), i32(_e3147.z), 0f);
                                    phi_32118_ = (_e3129 + (_e3153.y * _e3140));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e3158 = phi_32118_;
                                    phi_21860_ = _e3158;
                                    phi_21859_ = (_e3131 + 1i);
                                }
                            }
                            let _e3162 = local_52;
                            phi_21862_ = mix(_e3121, _e3162, _e3033.z);
                        }
                        let _e3165 = phi_21862_;
                        let _e3167 = ((_e2709 & 128u) != 0u);
                        phi_8268_ = _e3167;
                        if _e3167 {
                            phi_8268_ = (_e3033.w > 0.0001f);
                        }
                        let _e3171 = phi_8268_;
                        phi_21866_ = _e3165;
                        if _e3171 {
                            phi_21864_ = 0f;
                            phi_21863_ = 0i;
                            loop {
                                let _e3173 = phi_21864_;
                                let _e3175 = phi_21863_;
                                local_53 = _e3173;
                                if (_e3175 < _e2671) {
                                    let _e3179 = f32((1i << bitcast<u32>(_e3175)));
                                    let _e3184 = (1f - smoothstep((_e3179 * 15f), (_e3179 * 30f), _e164));
                                    if (_e3184 <= 0f) {
                                        phi_32119_ = _e3173;
                                        continue;
                                    }
                                    let _e3187 = (_e2614 / vec2((0.5f * _e3179)));
                                    let _e3191 = vec3<f32>(_e3187.x, _e3187.y, f32(_e3175));
                                    let _e3197 = textureSampleLevel(global_20, global_19, vec2<f32>(_e3191.x, _e3191.y), i32(_e3191.z), 0f);
                                    phi_32119_ = (_e3173 + (_e3197.y * _e3184));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e3202 = phi_32119_;
                                    phi_21864_ = _e3202;
                                    phi_21863_ = (_e3175 + 1i);
                                }
                            }
                            let _e3206 = local_53;
                            phi_21866_ = mix(_e3165, _e3206, _e3033.w);
                        }
                        let _e3209 = phi_21866_;
                        phi_21865_ = _e3209;
                    }
                    let _e3211 = phi_21865_;
                    let _e3214 = global.member_7[2u];
                    let _e3218 = global.member_7[3u];
                    phi_22046_ = (_e125 + clamp((_e3211 * _e3214), -(_e3218), _e3218));
                    break;
                }
            }
            let _e3223 = phi_22046_;
            let _e3228 = -(_e168);
            let _e3229 = reflect(_e3228, normalize(vec3<f32>((_e1393 - _e2003), 0.7f, (_e2613 - _e3223))));
            let _e3233 = global.member_4[3u];
            let _e3239 = (_e169 - ((_e3229.xz * max((_e119.y - _e781), 0f)) / vec2(max(_e3229.y, _e3233))));
            switch bitcast<i32>(0u) {
                default: {
                    let _e3242 = local_54;
                    let _e6763 = local_128;
                    phi_9817_ = _e6763;
                    if _e3242 {
                        let _e3245 = global.member_7[1u];
                        phi_9817_ = (_e3245 < 0.5f);
                    }
                    let _e3248 = phi_9817_;
                    if _e3248 {
                        let _e3251 = global.member_11[0u];
                        phi_23380_ = _e125;
                        phi_23379_ = 0i;
                        loop {
                            let _e3255 = phi_23380_;
                            let _e3257 = phi_23379_;
                            local_131 = _e3255;
                            if (_e3257 < i32(min(_e3251, 16f))) {
                                continue;
                            } else {
                                break;
                            }
                            continuing {
                                let _e3259 = (_e3257 * 2i);
                                let _e3262 = global.member_12[_e3259];
                                let _e3266 = global.member_12[(_e3259 + 1i)];
                                let _e3267 = _e3262.xy;
                                let _e3273 = max(_e3262.w, 0.001f);
                                let _e3274 = (6.2831855f / _e3273);
                                let _e3283 = global.member_2[0u];
                                phi_23380_ = (_e3255 + (_e3262.z * sin(((_e3274 * dot((_e3267 / vec2(max(length(_e3267), 0.0001f))), _e3239)) + (_e3283 * (_e3274 * (sqrt((1.56131f * _e3273)) * _e3266.y)))))));
                                phi_23379_ = (_e3257 + 1i);
                            }
                        }
                        let _e6836 = local_131;
                        phi_23381_ = _e6836;
                        break;
                    }
                    let _e3294 = global.member_7[1u];
                    let _e3296 = i32(min(_e3294, 16f));
                    phi_22048_ = 0f;
                    phi_22047_ = 0i;
                    loop {
                        let _e3298 = phi_22048_;
                        let _e3300 = phi_22047_;
                        local_55 = _e3298;
                        local_129 = _e3298;
                        local_130 = _e3298;
                        if (_e3300 < _e3296) {
                            let _e3304 = f32((1i << bitcast<u32>(_e3300)));
                            let _e3309 = (1f - smoothstep((_e3304 * 15f), (_e3304 * 30f), _e164));
                            if (_e3309 <= 0f) {
                                phi_32120_ = _e3298;
                                continue;
                            }
                            let _e3312 = (_e3239 / vec2((0.5f * _e3304)));
                            let _e3316 = vec3<f32>(_e3312.x, _e3312.y, f32(_e3300));
                            let _e3322 = textureSampleLevel(global_22, global_21, vec2<f32>(_e3316.x, _e3316.y), i32(_e3316.z), 0f);
                            phi_32120_ = (_e3298 + (_e3322.y * _e3309));
                            continue;
                        } else {
                            break;
                        }
                        continuing {
                            let _e3327 = phi_32120_;
                            phi_22048_ = _e3327;
                            phi_22047_ = (_e3300 + 1i);
                        }
                    }
                    let _e3331 = global.member_8[0u];
                    let _e3334 = u32((max(_e3331, 0f) + 0.5f));
                    let _e6833 = local_130;
                    phi_23200_ = _e6833;
                    if (_e3334 != 0u) {
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e3337 = (_e3334 == 0u);
                                local_60 = _e3337;
                                if _e3337 {
                                    phi_22056_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e3340 = global.member_10[0u];
                                let _e3343 = min(i32((_e3340 + 0.5f)), 7i);
                                if (_e3343 <= 0i) {
                                    phi_22056_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e3345 = textureDimensions(global_2, 0i);
                                let _e3348 = f32(vec2<i32>(_e3345).x);
                                phi_22049_ = 0i;
                                loop {
                                    let _e3350 = phi_22049_;
                                    phi_22057_ = vec4<f32>();
                                    phi_22052_ = false;
                                    if (_e3350 < 7i) {
                                        if (_e3350 >= _e3343) {
                                            phi_22057_ = vec4<f32>();
                                            phi_22052_ = false;
                                            break;
                                        }
                                        let _e3355 = global.member_9[_e3350];
                                        let _e3361 = ((_e3239 - _e3355.xy) / vec2((_e3355.z * _e3348)));
                                        let _e3363 = all((_e3361 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_10203_ = _e3363;
                                        if _e3363 {
                                            phi_10203_ = all((_e3361 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e3367 = phi_10203_;
                                        if _e3367 {
                                            let _e3371 = vec3<f32>(_e3361.x, _e3361.y, f32(_e3350));
                                            let _e3377 = textureSampleLevel(global_2, global_1, vec2<f32>(_e3371.x, _e3371.y), i32(_e3371.z), 0f);
                                            phi_22057_ = clamp(_e3377, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_22052_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_22049_ = (_e3350 + 1i);
                                    }
                                }
                                let _e3381 = phi_22057_;
                                let _e3383 = phi_22052_;
                                phi_22056_ = _e3381;
                                if _e3383 {
                                    break;
                                }
                                let _e3384 = (_e3343 - 1i);
                                let _e3387 = global.member_9[_e3384];
                                let _e3394 = clamp(((_e3239 - _e3387.xy) / vec2((_e3387.z * _e3348))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e3398 = vec3<f32>(_e3394.x, _e3394.y, f32(_e3384));
                                let _e3404 = textureSampleLevel(global_2, global_1, vec2<f32>(_e3398.x, _e3398.y), i32(_e3398.z), 0f);
                                phi_22056_ = clamp(_e3404, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e3407 = phi_22056_;
                        let _e3409 = ((_e3334 & 1u) != 0u);
                        phi_9839_ = _e3409;
                        if _e3409 {
                            phi_9839_ = (_e3407.x > 0.0001f);
                        }
                        let _e3413 = phi_9839_;
                        let _e6782 = local_129;
                        phi_22604_ = _e6782;
                        if _e3413 {
                            phi_22602_ = 0f;
                            phi_22601_ = 0i;
                            loop {
                                let _e3415 = phi_22602_;
                                let _e3417 = phi_22601_;
                                local_56 = _e3415;
                                if (_e3417 < _e3296) {
                                    let _e3421 = f32((1i << bitcast<u32>(_e3417)));
                                    let _e3426 = (1f - smoothstep((_e3421 * 15f), (_e3421 * 30f), _e164));
                                    if (_e3426 <= 0f) {
                                        phi_32122_ = _e3415;
                                        continue;
                                    }
                                    let _e3429 = (_e3239 / vec2((0.5f * _e3421)));
                                    let _e3433 = vec3<f32>(_e3429.x, _e3429.y, f32(_e3417));
                                    let _e3439 = textureSampleLevel(global_6, global_5, vec2<f32>(_e3433.x, _e3433.y), i32(_e3433.z), 0f);
                                    phi_32122_ = (_e3415 + (_e3439.y * _e3426));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e3444 = phi_32122_;
                                    phi_22602_ = _e3444;
                                    phi_22601_ = (_e3417 + 1i);
                                }
                            }
                            let _e3448 = local_55;
                            let _e3450 = local_56;
                            phi_22604_ = mix(_e3448, _e3450, _e3407.x);
                        }
                        let _e3453 = phi_22604_;
                        let _e3455 = ((_e3334 & 2u) != 0u);
                        phi_9857_ = _e3455;
                        if _e3455 {
                            phi_9857_ = (_e3407.y > 0.0001f);
                        }
                        let _e3459 = phi_9857_;
                        phi_22608_ = _e3453;
                        if _e3459 {
                            phi_22606_ = 0f;
                            phi_22605_ = 0i;
                            loop {
                                let _e3461 = phi_22606_;
                                let _e3463 = phi_22605_;
                                local_57 = _e3461;
                                if (_e3463 < _e3296) {
                                    let _e3467 = f32((1i << bitcast<u32>(_e3463)));
                                    let _e3472 = (1f - smoothstep((_e3467 * 15f), (_e3467 * 30f), _e164));
                                    if (_e3472 <= 0f) {
                                        phi_32123_ = _e3461;
                                        continue;
                                    }
                                    let _e3475 = (_e3239 / vec2((0.5f * _e3467)));
                                    let _e3479 = vec3<f32>(_e3475.x, _e3475.y, f32(_e3463));
                                    let _e3485 = textureSampleLevel(global_8, global_7, vec2<f32>(_e3479.x, _e3479.y), i32(_e3479.z), 0f);
                                    phi_32123_ = (_e3461 + (_e3485.y * _e3472));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e3490 = phi_32123_;
                                    phi_22606_ = _e3490;
                                    phi_22605_ = (_e3463 + 1i);
                                }
                            }
                            let _e3494 = local_57;
                            phi_22608_ = mix(_e3453, _e3494, _e3407.y);
                        }
                        let _e3497 = phi_22608_;
                        let _e3499 = ((_e3334 & 4u) != 0u);
                        phi_9875_ = _e3499;
                        if _e3499 {
                            phi_9875_ = (_e3407.z > 0.0001f);
                        }
                        let _e3503 = phi_9875_;
                        phi_22612_ = _e3497;
                        if _e3503 {
                            phi_22610_ = 0f;
                            phi_22609_ = 0i;
                            loop {
                                let _e3505 = phi_22610_;
                                let _e3507 = phi_22609_;
                                local_58 = _e3505;
                                if (_e3507 < _e3296) {
                                    let _e3511 = f32((1i << bitcast<u32>(_e3507)));
                                    let _e3516 = (1f - smoothstep((_e3511 * 15f), (_e3511 * 30f), _e164));
                                    if (_e3516 <= 0f) {
                                        phi_32124_ = _e3505;
                                        continue;
                                    }
                                    let _e3519 = (_e3239 / vec2((0.5f * _e3511)));
                                    let _e3523 = vec3<f32>(_e3519.x, _e3519.y, f32(_e3507));
                                    let _e3529 = textureSampleLevel(global_10, global_9, vec2<f32>(_e3523.x, _e3523.y), i32(_e3523.z), 0f);
                                    phi_32124_ = (_e3505 + (_e3529.y * _e3516));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e3534 = phi_32124_;
                                    phi_22610_ = _e3534;
                                    phi_22609_ = (_e3507 + 1i);
                                }
                            }
                            let _e3538 = local_58;
                            phi_22612_ = mix(_e3497, _e3538, _e3407.z);
                        }
                        let _e3541 = phi_22612_;
                        let _e3543 = ((_e3334 & 8u) != 0u);
                        phi_9893_ = _e3543;
                        if _e3543 {
                            phi_9893_ = (_e3407.w > 0.0001f);
                        }
                        let _e3547 = phi_9893_;
                        phi_23180_ = _e3541;
                        if _e3547 {
                            phi_22614_ = 0f;
                            phi_22613_ = 0i;
                            loop {
                                let _e3549 = phi_22614_;
                                let _e3551 = phi_22613_;
                                local_59 = _e3549;
                                if (_e3551 < _e3296) {
                                    let _e3555 = f32((1i << bitcast<u32>(_e3551)));
                                    let _e3560 = (1f - smoothstep((_e3555 * 15f), (_e3555 * 30f), _e164));
                                    if (_e3560 <= 0f) {
                                        phi_32125_ = _e3549;
                                        continue;
                                    }
                                    let _e3563 = (_e3239 / vec2((0.5f * _e3555)));
                                    let _e3567 = vec3<f32>(_e3563.x, _e3563.y, f32(_e3551));
                                    let _e3573 = textureSampleLevel(global_12, global_11, vec2<f32>(_e3567.x, _e3567.y), i32(_e3567.z), 0f);
                                    phi_32125_ = (_e3549 + (_e3573.y * _e3560));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e3578 = phi_32125_;
                                    phi_22614_ = _e3578;
                                    phi_22613_ = (_e3551 + 1i);
                                }
                            }
                            let _e3582 = local_59;
                            phi_23180_ = mix(_e3541, _e3582, _e3407.w);
                        }
                        let _e3585 = phi_23180_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e3588 = local_60;
                                if _e3588 {
                                    phi_22622_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e3591 = global.member_10[0u];
                                let _e3594 = min(i32((_e3591 + 0.5f)), 7i);
                                if (_e3594 <= 0i) {
                                    phi_22622_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e3596 = textureDimensions(global_2, 0i);
                                let _e3599 = f32(vec2<i32>(_e3596).x);
                                phi_22615_ = 0i;
                                loop {
                                    let _e3601 = phi_22615_;
                                    phi_22623_ = vec4<f32>();
                                    phi_22618_ = false;
                                    if (_e3601 < 7i) {
                                        if (_e3601 >= _e3594) {
                                            phi_22623_ = vec4<f32>();
                                            phi_22618_ = false;
                                            break;
                                        }
                                        let _e3606 = global.member_9[_e3601];
                                        let _e3612 = ((_e3239 - _e3606.xy) / vec2((_e3606.z * _e3599)));
                                        let _e3614 = all((_e3612 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_10870_ = _e3614;
                                        if _e3614 {
                                            phi_10870_ = all((_e3612 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e3618 = phi_10870_;
                                        if _e3618 {
                                            let _e3622 = vec3<f32>(_e3612.x, _e3612.y, f32(_e3601));
                                            let _e3628 = textureSampleLevel(global_4, global_3, vec2<f32>(_e3622.x, _e3622.y), i32(_e3622.z), 0f);
                                            phi_22623_ = clamp(_e3628, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_22618_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_22615_ = (_e3601 + 1i);
                                    }
                                }
                                let _e3632 = phi_22623_;
                                let _e3634 = phi_22618_;
                                phi_22622_ = _e3632;
                                if _e3634 {
                                    break;
                                }
                                let _e3635 = (_e3594 - 1i);
                                let _e3638 = global.member_9[_e3635];
                                let _e3645 = clamp(((_e3239 - _e3638.xy) / vec2((_e3638.z * _e3599))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e3649 = vec3<f32>(_e3645.x, _e3645.y, f32(_e3635));
                                let _e3655 = textureSampleLevel(global_4, global_3, vec2<f32>(_e3649.x, _e3649.y), i32(_e3649.z), 0f);
                                phi_22622_ = clamp(_e3655, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e3658 = phi_22622_;
                        let _e3660 = ((_e3334 & 16u) != 0u);
                        phi_9913_ = _e3660;
                        if _e3660 {
                            phi_9913_ = (_e3658.x > 0.0001f);
                        }
                        let _e3664 = phi_9913_;
                        phi_23189_ = _e3585;
                        if _e3664 {
                            phi_23187_ = 0f;
                            phi_23186_ = 0i;
                            loop {
                                let _e3666 = phi_23187_;
                                let _e3668 = phi_23186_;
                                local_61 = _e3666;
                                if (_e3668 < _e3296) {
                                    let _e3672 = f32((1i << bitcast<u32>(_e3668)));
                                    let _e3677 = (1f - smoothstep((_e3672 * 15f), (_e3672 * 30f), _e164));
                                    if (_e3677 <= 0f) {
                                        phi_32159_ = _e3666;
                                        continue;
                                    }
                                    let _e3680 = (_e3239 / vec2((0.5f * _e3672)));
                                    let _e3684 = vec3<f32>(_e3680.x, _e3680.y, f32(_e3668));
                                    let _e3690 = textureSampleLevel(global_14, global_13, vec2<f32>(_e3684.x, _e3684.y), i32(_e3684.z), 0f);
                                    phi_32159_ = (_e3666 + (_e3690.y * _e3677));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e3695 = phi_32159_;
                                    phi_23187_ = _e3695;
                                    phi_23186_ = (_e3668 + 1i);
                                }
                            }
                            let _e3699 = local_61;
                            phi_23189_ = mix(_e3585, _e3699, _e3658.x);
                        }
                        let _e3702 = phi_23189_;
                        let _e3704 = ((_e3334 & 32u) != 0u);
                        phi_9931_ = _e3704;
                        if _e3704 {
                            phi_9931_ = (_e3658.y > 0.0001f);
                        }
                        let _e3708 = phi_9931_;
                        phi_23193_ = _e3702;
                        if _e3708 {
                            phi_23191_ = 0f;
                            phi_23190_ = 0i;
                            loop {
                                let _e3710 = phi_23191_;
                                let _e3712 = phi_23190_;
                                local_62 = _e3710;
                                if (_e3712 < _e3296) {
                                    let _e3716 = f32((1i << bitcast<u32>(_e3712)));
                                    let _e3721 = (1f - smoothstep((_e3716 * 15f), (_e3716 * 30f), _e164));
                                    if (_e3721 <= 0f) {
                                        phi_32160_ = _e3710;
                                        continue;
                                    }
                                    let _e3724 = (_e3239 / vec2((0.5f * _e3716)));
                                    let _e3728 = vec3<f32>(_e3724.x, _e3724.y, f32(_e3712));
                                    let _e3734 = textureSampleLevel(global_16, global_15, vec2<f32>(_e3728.x, _e3728.y), i32(_e3728.z), 0f);
                                    phi_32160_ = (_e3710 + (_e3734.y * _e3721));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e3739 = phi_32160_;
                                    phi_23191_ = _e3739;
                                    phi_23190_ = (_e3712 + 1i);
                                }
                            }
                            let _e3743 = local_62;
                            phi_23193_ = mix(_e3702, _e3743, _e3658.y);
                        }
                        let _e3746 = phi_23193_;
                        let _e3748 = ((_e3334 & 64u) != 0u);
                        phi_9949_ = _e3748;
                        if _e3748 {
                            phi_9949_ = (_e3658.z > 0.0001f);
                        }
                        let _e3752 = phi_9949_;
                        phi_23197_ = _e3746;
                        if _e3752 {
                            phi_23195_ = 0f;
                            phi_23194_ = 0i;
                            loop {
                                let _e3754 = phi_23195_;
                                let _e3756 = phi_23194_;
                                local_63 = _e3754;
                                if (_e3756 < _e3296) {
                                    let _e3760 = f32((1i << bitcast<u32>(_e3756)));
                                    let _e3765 = (1f - smoothstep((_e3760 * 15f), (_e3760 * 30f), _e164));
                                    if (_e3765 <= 0f) {
                                        phi_32161_ = _e3754;
                                        continue;
                                    }
                                    let _e3768 = (_e3239 / vec2((0.5f * _e3760)));
                                    let _e3772 = vec3<f32>(_e3768.x, _e3768.y, f32(_e3756));
                                    let _e3778 = textureSampleLevel(global_18, global_17, vec2<f32>(_e3772.x, _e3772.y), i32(_e3772.z), 0f);
                                    phi_32161_ = (_e3754 + (_e3778.y * _e3765));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e3783 = phi_32161_;
                                    phi_23195_ = _e3783;
                                    phi_23194_ = (_e3756 + 1i);
                                }
                            }
                            let _e3787 = local_63;
                            phi_23197_ = mix(_e3746, _e3787, _e3658.z);
                        }
                        let _e3790 = phi_23197_;
                        let _e3792 = ((_e3334 & 128u) != 0u);
                        phi_9967_ = _e3792;
                        if _e3792 {
                            phi_9967_ = (_e3658.w > 0.0001f);
                        }
                        let _e3796 = phi_9967_;
                        phi_23201_ = _e3790;
                        if _e3796 {
                            phi_23199_ = 0f;
                            phi_23198_ = 0i;
                            loop {
                                let _e3798 = phi_23199_;
                                let _e3800 = phi_23198_;
                                local_64 = _e3798;
                                if (_e3800 < _e3296) {
                                    let _e3804 = f32((1i << bitcast<u32>(_e3800)));
                                    let _e3809 = (1f - smoothstep((_e3804 * 15f), (_e3804 * 30f), _e164));
                                    if (_e3809 <= 0f) {
                                        phi_32162_ = _e3798;
                                        continue;
                                    }
                                    let _e3812 = (_e3239 / vec2((0.5f * _e3804)));
                                    let _e3816 = vec3<f32>(_e3812.x, _e3812.y, f32(_e3800));
                                    let _e3822 = textureSampleLevel(global_20, global_19, vec2<f32>(_e3816.x, _e3816.y), i32(_e3816.z), 0f);
                                    phi_32162_ = (_e3798 + (_e3822.y * _e3809));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e3827 = phi_32162_;
                                    phi_23199_ = _e3827;
                                    phi_23198_ = (_e3800 + 1i);
                                }
                            }
                            let _e3831 = local_64;
                            phi_23201_ = mix(_e3790, _e3831, _e3658.w);
                        }
                        let _e3834 = phi_23201_;
                        phi_23200_ = _e3834;
                    }
                    let _e3836 = phi_23200_;
                    let _e3839 = global.member_7[2u];
                    let _e3843 = global.member_7[3u];
                    phi_23381_ = (_e125 + clamp((_e3836 * _e3839), -(_e3843), _e3843));
                    break;
                }
            }
            let _e3848 = phi_23381_;
            let _e3849 = (_e3239 - vec2<f32>(0.35f, 0f));
            switch bitcast<i32>(0u) {
                default: {
                    let _e3852 = local_65;
                    let _e6840 = local_132;
                    phi_11565_ = _e6840;
                    if _e3852 {
                        let _e3855 = global.member_7[1u];
                        phi_11565_ = (_e3855 < 0.5f);
                    }
                    let _e3858 = phi_11565_;
                    if _e3858 {
                        let _e3861 = global.member_11[0u];
                        phi_24927_ = _e125;
                        phi_24926_ = 0i;
                        loop {
                            let _e3865 = phi_24927_;
                            let _e3867 = phi_24926_;
                            local_135 = _e3865;
                            if (_e3867 < i32(min(_e3861, 16f))) {
                                continue;
                            } else {
                                break;
                            }
                            continuing {
                                let _e3869 = (_e3867 * 2i);
                                let _e3872 = global.member_12[_e3869];
                                let _e3876 = global.member_12[(_e3869 + 1i)];
                                let _e3877 = _e3872.xy;
                                let _e3883 = max(_e3872.w, 0.001f);
                                let _e3884 = (6.2831855f / _e3883);
                                let _e3893 = global.member_2[0u];
                                phi_24927_ = (_e3865 + (_e3872.z * sin(((_e3884 * dot((_e3877 / vec2(max(length(_e3877), 0.0001f))), _e3849)) + (_e3893 * (_e3884 * (sqrt((1.56131f * _e3883)) * _e3876.y)))))));
                                phi_24926_ = (_e3867 + 1i);
                            }
                        }
                        let _e6913 = local_135;
                        phi_24928_ = _e6913;
                        break;
                    }
                    let _e3904 = global.member_7[1u];
                    let _e3906 = i32(min(_e3904, 16f));
                    phi_23383_ = 0f;
                    phi_23382_ = 0i;
                    loop {
                        let _e3908 = phi_23383_;
                        let _e3910 = phi_23382_;
                        local_66 = _e3908;
                        local_133 = _e3908;
                        local_134 = _e3908;
                        if (_e3910 < _e3906) {
                            let _e3914 = f32((1i << bitcast<u32>(_e3910)));
                            let _e3919 = (1f - smoothstep((_e3914 * 15f), (_e3914 * 30f), _e164));
                            if (_e3919 <= 0f) {
                                phi_32163_ = _e3908;
                                continue;
                            }
                            let _e3922 = (_e3849 / vec2((0.5f * _e3914)));
                            let _e3926 = vec3<f32>(_e3922.x, _e3922.y, f32(_e3910));
                            let _e3932 = textureSampleLevel(global_22, global_21, vec2<f32>(_e3926.x, _e3926.y), i32(_e3926.z), 0f);
                            phi_32163_ = (_e3908 + (_e3932.y * _e3919));
                            continue;
                        } else {
                            break;
                        }
                        continuing {
                            let _e3937 = phi_32163_;
                            phi_23383_ = _e3937;
                            phi_23382_ = (_e3910 + 1i);
                        }
                    }
                    let _e3941 = global.member_8[0u];
                    let _e3944 = u32((max(_e3941, 0f) + 0.5f));
                    let _e6910 = local_134;
                    phi_24747_ = _e6910;
                    if (_e3944 != 0u) {
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e3947 = (_e3944 == 0u);
                                local_71 = _e3947;
                                if _e3947 {
                                    phi_23391_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e3950 = global.member_10[0u];
                                let _e3953 = min(i32((_e3950 + 0.5f)), 7i);
                                if (_e3953 <= 0i) {
                                    phi_23391_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e3955 = textureDimensions(global_2, 0i);
                                let _e3958 = f32(vec2<i32>(_e3955).x);
                                phi_23384_ = 0i;
                                loop {
                                    let _e3960 = phi_23384_;
                                    phi_23392_ = vec4<f32>();
                                    phi_23387_ = false;
                                    if (_e3960 < 7i) {
                                        if (_e3960 >= _e3953) {
                                            phi_23392_ = vec4<f32>();
                                            phi_23387_ = false;
                                            break;
                                        }
                                        let _e3965 = global.member_9[_e3960];
                                        let _e3971 = ((_e3849 - _e3965.xy) / vec2((_e3965.z * _e3958)));
                                        let _e3973 = all((_e3971 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_11951_ = _e3973;
                                        if _e3973 {
                                            phi_11951_ = all((_e3971 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e3977 = phi_11951_;
                                        if _e3977 {
                                            let _e3981 = vec3<f32>(_e3971.x, _e3971.y, f32(_e3960));
                                            let _e3987 = textureSampleLevel(global_2, global_1, vec2<f32>(_e3981.x, _e3981.y), i32(_e3981.z), 0f);
                                            phi_23392_ = clamp(_e3987, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_23387_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_23384_ = (_e3960 + 1i);
                                    }
                                }
                                let _e3991 = phi_23392_;
                                let _e3993 = phi_23387_;
                                phi_23391_ = _e3991;
                                if _e3993 {
                                    break;
                                }
                                let _e3994 = (_e3953 - 1i);
                                let _e3997 = global.member_9[_e3994];
                                let _e4004 = clamp(((_e3849 - _e3997.xy) / vec2((_e3997.z * _e3958))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e4008 = vec3<f32>(_e4004.x, _e4004.y, f32(_e3994));
                                let _e4014 = textureSampleLevel(global_2, global_1, vec2<f32>(_e4008.x, _e4008.y), i32(_e4008.z), 0f);
                                phi_23391_ = clamp(_e4014, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e4017 = phi_23391_;
                        let _e4019 = ((_e3944 & 1u) != 0u);
                        phi_11587_ = _e4019;
                        if _e4019 {
                            phi_11587_ = (_e4017.x > 0.0001f);
                        }
                        let _e4023 = phi_11587_;
                        let _e6859 = local_133;
                        phi_24045_ = _e6859;
                        if _e4023 {
                            phi_24043_ = 0f;
                            phi_24042_ = 0i;
                            loop {
                                let _e4025 = phi_24043_;
                                let _e4027 = phi_24042_;
                                local_67 = _e4025;
                                if (_e4027 < _e3906) {
                                    let _e4031 = f32((1i << bitcast<u32>(_e4027)));
                                    let _e4036 = (1f - smoothstep((_e4031 * 15f), (_e4031 * 30f), _e164));
                                    if (_e4036 <= 0f) {
                                        phi_32165_ = _e4025;
                                        continue;
                                    }
                                    let _e4039 = (_e3849 / vec2((0.5f * _e4031)));
                                    let _e4043 = vec3<f32>(_e4039.x, _e4039.y, f32(_e4027));
                                    let _e4049 = textureSampleLevel(global_6, global_5, vec2<f32>(_e4043.x, _e4043.y), i32(_e4043.z), 0f);
                                    phi_32165_ = (_e4025 + (_e4049.y * _e4036));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4054 = phi_32165_;
                                    phi_24043_ = _e4054;
                                    phi_24042_ = (_e4027 + 1i);
                                }
                            }
                            let _e4058 = local_66;
                            let _e4060 = local_67;
                            phi_24045_ = mix(_e4058, _e4060, _e4017.x);
                        }
                        let _e4063 = phi_24045_;
                        let _e4065 = ((_e3944 & 2u) != 0u);
                        phi_11605_ = _e4065;
                        if _e4065 {
                            phi_11605_ = (_e4017.y > 0.0001f);
                        }
                        let _e4069 = phi_11605_;
                        phi_24049_ = _e4063;
                        if _e4069 {
                            phi_24047_ = 0f;
                            phi_24046_ = 0i;
                            loop {
                                let _e4071 = phi_24047_;
                                let _e4073 = phi_24046_;
                                local_68 = _e4071;
                                if (_e4073 < _e3906) {
                                    let _e4077 = f32((1i << bitcast<u32>(_e4073)));
                                    let _e4082 = (1f - smoothstep((_e4077 * 15f), (_e4077 * 30f), _e164));
                                    if (_e4082 <= 0f) {
                                        phi_32166_ = _e4071;
                                        continue;
                                    }
                                    let _e4085 = (_e3849 / vec2((0.5f * _e4077)));
                                    let _e4089 = vec3<f32>(_e4085.x, _e4085.y, f32(_e4073));
                                    let _e4095 = textureSampleLevel(global_8, global_7, vec2<f32>(_e4089.x, _e4089.y), i32(_e4089.z), 0f);
                                    phi_32166_ = (_e4071 + (_e4095.y * _e4082));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4100 = phi_32166_;
                                    phi_24047_ = _e4100;
                                    phi_24046_ = (_e4073 + 1i);
                                }
                            }
                            let _e4104 = local_68;
                            phi_24049_ = mix(_e4063, _e4104, _e4017.y);
                        }
                        let _e4107 = phi_24049_;
                        let _e4109 = ((_e3944 & 4u) != 0u);
                        phi_11623_ = _e4109;
                        if _e4109 {
                            phi_11623_ = (_e4017.z > 0.0001f);
                        }
                        let _e4113 = phi_11623_;
                        phi_24053_ = _e4107;
                        if _e4113 {
                            phi_24051_ = 0f;
                            phi_24050_ = 0i;
                            loop {
                                let _e4115 = phi_24051_;
                                let _e4117 = phi_24050_;
                                local_69 = _e4115;
                                if (_e4117 < _e3906) {
                                    let _e4121 = f32((1i << bitcast<u32>(_e4117)));
                                    let _e4126 = (1f - smoothstep((_e4121 * 15f), (_e4121 * 30f), _e164));
                                    if (_e4126 <= 0f) {
                                        phi_32167_ = _e4115;
                                        continue;
                                    }
                                    let _e4129 = (_e3849 / vec2((0.5f * _e4121)));
                                    let _e4133 = vec3<f32>(_e4129.x, _e4129.y, f32(_e4117));
                                    let _e4139 = textureSampleLevel(global_10, global_9, vec2<f32>(_e4133.x, _e4133.y), i32(_e4133.z), 0f);
                                    phi_32167_ = (_e4115 + (_e4139.y * _e4126));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4144 = phi_32167_;
                                    phi_24051_ = _e4144;
                                    phi_24050_ = (_e4117 + 1i);
                                }
                            }
                            let _e4148 = local_69;
                            phi_24053_ = mix(_e4107, _e4148, _e4017.z);
                        }
                        let _e4151 = phi_24053_;
                        let _e4153 = ((_e3944 & 8u) != 0u);
                        phi_11641_ = _e4153;
                        if _e4153 {
                            phi_11641_ = (_e4017.w > 0.0001f);
                        }
                        let _e4157 = phi_11641_;
                        phi_24727_ = _e4151;
                        if _e4157 {
                            phi_24055_ = 0f;
                            phi_24054_ = 0i;
                            loop {
                                let _e4159 = phi_24055_;
                                let _e4161 = phi_24054_;
                                local_70 = _e4159;
                                if (_e4161 < _e3906) {
                                    let _e4165 = f32((1i << bitcast<u32>(_e4161)));
                                    let _e4170 = (1f - smoothstep((_e4165 * 15f), (_e4165 * 30f), _e164));
                                    if (_e4170 <= 0f) {
                                        phi_32168_ = _e4159;
                                        continue;
                                    }
                                    let _e4173 = (_e3849 / vec2((0.5f * _e4165)));
                                    let _e4177 = vec3<f32>(_e4173.x, _e4173.y, f32(_e4161));
                                    let _e4183 = textureSampleLevel(global_12, global_11, vec2<f32>(_e4177.x, _e4177.y), i32(_e4177.z), 0f);
                                    phi_32168_ = (_e4159 + (_e4183.y * _e4170));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4188 = phi_32168_;
                                    phi_24055_ = _e4188;
                                    phi_24054_ = (_e4161 + 1i);
                                }
                            }
                            let _e4192 = local_70;
                            phi_24727_ = mix(_e4151, _e4192, _e4017.w);
                        }
                        let _e4195 = phi_24727_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e4198 = local_71;
                                if _e4198 {
                                    phi_24063_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e4201 = global.member_10[0u];
                                let _e4204 = min(i32((_e4201 + 0.5f)), 7i);
                                if (_e4204 <= 0i) {
                                    phi_24063_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e4206 = textureDimensions(global_2, 0i);
                                let _e4209 = f32(vec2<i32>(_e4206).x);
                                phi_24056_ = 0i;
                                loop {
                                    let _e4211 = phi_24056_;
                                    phi_24064_ = vec4<f32>();
                                    phi_24059_ = false;
                                    if (_e4211 < 7i) {
                                        if (_e4211 >= _e4204) {
                                            phi_24064_ = vec4<f32>();
                                            phi_24059_ = false;
                                            break;
                                        }
                                        let _e4216 = global.member_9[_e4211];
                                        let _e4222 = ((_e3849 - _e4216.xy) / vec2((_e4216.z * _e4209)));
                                        let _e4224 = all((_e4222 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_12618_ = _e4224;
                                        if _e4224 {
                                            phi_12618_ = all((_e4222 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e4228 = phi_12618_;
                                        if _e4228 {
                                            let _e4232 = vec3<f32>(_e4222.x, _e4222.y, f32(_e4211));
                                            let _e4238 = textureSampleLevel(global_4, global_3, vec2<f32>(_e4232.x, _e4232.y), i32(_e4232.z), 0f);
                                            phi_24064_ = clamp(_e4238, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_24059_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_24056_ = (_e4211 + 1i);
                                    }
                                }
                                let _e4242 = phi_24064_;
                                let _e4244 = phi_24059_;
                                phi_24063_ = _e4242;
                                if _e4244 {
                                    break;
                                }
                                let _e4245 = (_e4204 - 1i);
                                let _e4248 = global.member_9[_e4245];
                                let _e4255 = clamp(((_e3849 - _e4248.xy) / vec2((_e4248.z * _e4209))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e4259 = vec3<f32>(_e4255.x, _e4255.y, f32(_e4245));
                                let _e4265 = textureSampleLevel(global_4, global_3, vec2<f32>(_e4259.x, _e4259.y), i32(_e4259.z), 0f);
                                phi_24063_ = clamp(_e4265, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e4268 = phi_24063_;
                        let _e4270 = ((_e3944 & 16u) != 0u);
                        phi_11661_ = _e4270;
                        if _e4270 {
                            phi_11661_ = (_e4268.x > 0.0001f);
                        }
                        let _e4274 = phi_11661_;
                        phi_24736_ = _e4195;
                        if _e4274 {
                            phi_24734_ = 0f;
                            phi_24733_ = 0i;
                            loop {
                                let _e4276 = phi_24734_;
                                let _e4278 = phi_24733_;
                                local_72 = _e4276;
                                if (_e4278 < _e3906) {
                                    let _e4282 = f32((1i << bitcast<u32>(_e4278)));
                                    let _e4287 = (1f - smoothstep((_e4282 * 15f), (_e4282 * 30f), _e164));
                                    if (_e4287 <= 0f) {
                                        phi_32202_ = _e4276;
                                        continue;
                                    }
                                    let _e4290 = (_e3849 / vec2((0.5f * _e4282)));
                                    let _e4294 = vec3<f32>(_e4290.x, _e4290.y, f32(_e4278));
                                    let _e4300 = textureSampleLevel(global_14, global_13, vec2<f32>(_e4294.x, _e4294.y), i32(_e4294.z), 0f);
                                    phi_32202_ = (_e4276 + (_e4300.y * _e4287));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4305 = phi_32202_;
                                    phi_24734_ = _e4305;
                                    phi_24733_ = (_e4278 + 1i);
                                }
                            }
                            let _e4309 = local_72;
                            phi_24736_ = mix(_e4195, _e4309, _e4268.x);
                        }
                        let _e4312 = phi_24736_;
                        let _e4314 = ((_e3944 & 32u) != 0u);
                        phi_11679_ = _e4314;
                        if _e4314 {
                            phi_11679_ = (_e4268.y > 0.0001f);
                        }
                        let _e4318 = phi_11679_;
                        phi_24740_ = _e4312;
                        if _e4318 {
                            phi_24738_ = 0f;
                            phi_24737_ = 0i;
                            loop {
                                let _e4320 = phi_24738_;
                                let _e4322 = phi_24737_;
                                local_73 = _e4320;
                                if (_e4322 < _e3906) {
                                    let _e4326 = f32((1i << bitcast<u32>(_e4322)));
                                    let _e4331 = (1f - smoothstep((_e4326 * 15f), (_e4326 * 30f), _e164));
                                    if (_e4331 <= 0f) {
                                        phi_32203_ = _e4320;
                                        continue;
                                    }
                                    let _e4334 = (_e3849 / vec2((0.5f * _e4326)));
                                    let _e4338 = vec3<f32>(_e4334.x, _e4334.y, f32(_e4322));
                                    let _e4344 = textureSampleLevel(global_16, global_15, vec2<f32>(_e4338.x, _e4338.y), i32(_e4338.z), 0f);
                                    phi_32203_ = (_e4320 + (_e4344.y * _e4331));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4349 = phi_32203_;
                                    phi_24738_ = _e4349;
                                    phi_24737_ = (_e4322 + 1i);
                                }
                            }
                            let _e4353 = local_73;
                            phi_24740_ = mix(_e4312, _e4353, _e4268.y);
                        }
                        let _e4356 = phi_24740_;
                        let _e4358 = ((_e3944 & 64u) != 0u);
                        phi_11697_ = _e4358;
                        if _e4358 {
                            phi_11697_ = (_e4268.z > 0.0001f);
                        }
                        let _e4362 = phi_11697_;
                        phi_24744_ = _e4356;
                        if _e4362 {
                            phi_24742_ = 0f;
                            phi_24741_ = 0i;
                            loop {
                                let _e4364 = phi_24742_;
                                let _e4366 = phi_24741_;
                                local_74 = _e4364;
                                if (_e4366 < _e3906) {
                                    let _e4370 = f32((1i << bitcast<u32>(_e4366)));
                                    let _e4375 = (1f - smoothstep((_e4370 * 15f), (_e4370 * 30f), _e164));
                                    if (_e4375 <= 0f) {
                                        phi_32204_ = _e4364;
                                        continue;
                                    }
                                    let _e4378 = (_e3849 / vec2((0.5f * _e4370)));
                                    let _e4382 = vec3<f32>(_e4378.x, _e4378.y, f32(_e4366));
                                    let _e4388 = textureSampleLevel(global_18, global_17, vec2<f32>(_e4382.x, _e4382.y), i32(_e4382.z), 0f);
                                    phi_32204_ = (_e4364 + (_e4388.y * _e4375));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4393 = phi_32204_;
                                    phi_24742_ = _e4393;
                                    phi_24741_ = (_e4366 + 1i);
                                }
                            }
                            let _e4397 = local_74;
                            phi_24744_ = mix(_e4356, _e4397, _e4268.z);
                        }
                        let _e4400 = phi_24744_;
                        let _e4402 = ((_e3944 & 128u) != 0u);
                        phi_11715_ = _e4402;
                        if _e4402 {
                            phi_11715_ = (_e4268.w > 0.0001f);
                        }
                        let _e4406 = phi_11715_;
                        phi_24748_ = _e4400;
                        if _e4406 {
                            phi_24746_ = 0f;
                            phi_24745_ = 0i;
                            loop {
                                let _e4408 = phi_24746_;
                                let _e4410 = phi_24745_;
                                local_75 = _e4408;
                                if (_e4410 < _e3906) {
                                    let _e4414 = f32((1i << bitcast<u32>(_e4410)));
                                    let _e4419 = (1f - smoothstep((_e4414 * 15f), (_e4414 * 30f), _e164));
                                    if (_e4419 <= 0f) {
                                        phi_32205_ = _e4408;
                                        continue;
                                    }
                                    let _e4422 = (_e3849 / vec2((0.5f * _e4414)));
                                    let _e4426 = vec3<f32>(_e4422.x, _e4422.y, f32(_e4410));
                                    let _e4432 = textureSampleLevel(global_20, global_19, vec2<f32>(_e4426.x, _e4426.y), i32(_e4426.z), 0f);
                                    phi_32205_ = (_e4408 + (_e4432.y * _e4419));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4437 = phi_32205_;
                                    phi_24746_ = _e4437;
                                    phi_24745_ = (_e4410 + 1i);
                                }
                            }
                            let _e4441 = local_75;
                            phi_24748_ = mix(_e4400, _e4441, _e4268.w);
                        }
                        let _e4444 = phi_24748_;
                        phi_24747_ = _e4444;
                    }
                    let _e4446 = phi_24747_;
                    let _e4449 = global.member_7[2u];
                    let _e4453 = global.member_7[3u];
                    phi_24928_ = (_e125 + clamp((_e4446 * _e4449), -(_e4453), _e4453));
                    break;
                }
            }
            let _e4458 = phi_24928_;
            let _e4459 = (_e3239 + vec2<f32>(0.35f, 0f));
            switch bitcast<i32>(0u) {
                default: {
                    let _e4462 = local_76;
                    let _e6917 = local_136;
                    phi_13264_ = _e6917;
                    if _e4462 {
                        let _e4465 = global.member_7[1u];
                        phi_13264_ = (_e4465 < 0.5f);
                    }
                    let _e4468 = phi_13264_;
                    if _e4468 {
                        let _e4471 = global.member_11[0u];
                        phi_26686_ = _e125;
                        phi_26685_ = 0i;
                        loop {
                            let _e4475 = phi_26686_;
                            let _e4477 = phi_26685_;
                            local_139 = _e4475;
                            if (_e4477 < i32(min(_e4471, 16f))) {
                                continue;
                            } else {
                                break;
                            }
                            continuing {
                                let _e4479 = (_e4477 * 2i);
                                let _e4482 = global.member_12[_e4479];
                                let _e4486 = global.member_12[(_e4479 + 1i)];
                                let _e4487 = _e4482.xy;
                                let _e4493 = max(_e4482.w, 0.001f);
                                let _e4494 = (6.2831855f / _e4493);
                                let _e4503 = global.member_2[0u];
                                phi_26686_ = (_e4475 + (_e4482.z * sin(((_e4494 * dot((_e4487 / vec2(max(length(_e4487), 0.0001f))), _e4459)) + (_e4503 * (_e4494 * (sqrt((1.56131f * _e4493)) * _e4486.y)))))));
                                phi_26685_ = (_e4477 + 1i);
                            }
                        }
                        let _e6990 = local_139;
                        phi_26687_ = _e6990;
                        break;
                    }
                    let _e4514 = global.member_7[1u];
                    let _e4516 = i32(min(_e4514, 16f));
                    phi_24930_ = 0f;
                    phi_24929_ = 0i;
                    loop {
                        let _e4518 = phi_24930_;
                        let _e4520 = phi_24929_;
                        local_77 = _e4518;
                        local_137 = _e4518;
                        local_138 = _e4518;
                        if (_e4520 < _e4516) {
                            let _e4524 = f32((1i << bitcast<u32>(_e4520)));
                            let _e4529 = (1f - smoothstep((_e4524 * 15f), (_e4524 * 30f), _e164));
                            if (_e4529 <= 0f) {
                                phi_32206_ = _e4518;
                                continue;
                            }
                            let _e4532 = (_e4459 / vec2((0.5f * _e4524)));
                            let _e4536 = vec3<f32>(_e4532.x, _e4532.y, f32(_e4520));
                            let _e4542 = textureSampleLevel(global_22, global_21, vec2<f32>(_e4536.x, _e4536.y), i32(_e4536.z), 0f);
                            phi_32206_ = (_e4518 + (_e4542.y * _e4529));
                            continue;
                        } else {
                            break;
                        }
                        continuing {
                            let _e4547 = phi_32206_;
                            phi_24930_ = _e4547;
                            phi_24929_ = (_e4520 + 1i);
                        }
                    }
                    let _e4551 = global.member_8[0u];
                    let _e4554 = u32((max(_e4551, 0f) + 0.5f));
                    let _e6987 = local_138;
                    phi_26506_ = _e6987;
                    if (_e4554 != 0u) {
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e4557 = (_e4554 == 0u);
                                local_82 = _e4557;
                                if _e4557 {
                                    phi_24938_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e4560 = global.member_10[0u];
                                let _e4563 = min(i32((_e4560 + 0.5f)), 7i);
                                if (_e4563 <= 0i) {
                                    phi_24938_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e4565 = textureDimensions(global_2, 0i);
                                let _e4568 = f32(vec2<i32>(_e4565).x);
                                phi_24931_ = 0i;
                                loop {
                                    let _e4570 = phi_24931_;
                                    phi_24939_ = vec4<f32>();
                                    phi_24934_ = false;
                                    if (_e4570 < 7i) {
                                        if (_e4570 >= _e4563) {
                                            phi_24939_ = vec4<f32>();
                                            phi_24934_ = false;
                                            break;
                                        }
                                        let _e4575 = global.member_9[_e4570];
                                        let _e4581 = ((_e4459 - _e4575.xy) / vec2((_e4575.z * _e4568)));
                                        let _e4583 = all((_e4581 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_13650_ = _e4583;
                                        if _e4583 {
                                            phi_13650_ = all((_e4581 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e4587 = phi_13650_;
                                        if _e4587 {
                                            let _e4591 = vec3<f32>(_e4581.x, _e4581.y, f32(_e4570));
                                            let _e4597 = textureSampleLevel(global_2, global_1, vec2<f32>(_e4591.x, _e4591.y), i32(_e4591.z), 0f);
                                            phi_24939_ = clamp(_e4597, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_24934_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_24931_ = (_e4570 + 1i);
                                    }
                                }
                                let _e4601 = phi_24939_;
                                let _e4603 = phi_24934_;
                                phi_24938_ = _e4601;
                                if _e4603 {
                                    break;
                                }
                                let _e4604 = (_e4563 - 1i);
                                let _e4607 = global.member_9[_e4604];
                                let _e4614 = clamp(((_e4459 - _e4607.xy) / vec2((_e4607.z * _e4568))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e4618 = vec3<f32>(_e4614.x, _e4614.y, f32(_e4604));
                                let _e4624 = textureSampleLevel(global_2, global_1, vec2<f32>(_e4618.x, _e4618.y), i32(_e4618.z), 0f);
                                phi_24938_ = clamp(_e4624, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e4627 = phi_24938_;
                        let _e4629 = ((_e4554 & 1u) != 0u);
                        phi_13286_ = _e4629;
                        if _e4629 {
                            phi_13286_ = (_e4627.x > 0.0001f);
                        }
                        let _e4633 = phi_13286_;
                        let _e6936 = local_137;
                        phi_25698_ = _e6936;
                        if _e4633 {
                            phi_25696_ = 0f;
                            phi_25695_ = 0i;
                            loop {
                                let _e4635 = phi_25696_;
                                let _e4637 = phi_25695_;
                                local_78 = _e4635;
                                if (_e4637 < _e4516) {
                                    let _e4641 = f32((1i << bitcast<u32>(_e4637)));
                                    let _e4646 = (1f - smoothstep((_e4641 * 15f), (_e4641 * 30f), _e164));
                                    if (_e4646 <= 0f) {
                                        phi_32208_ = _e4635;
                                        continue;
                                    }
                                    let _e4649 = (_e4459 / vec2((0.5f * _e4641)));
                                    let _e4653 = vec3<f32>(_e4649.x, _e4649.y, f32(_e4637));
                                    let _e4659 = textureSampleLevel(global_6, global_5, vec2<f32>(_e4653.x, _e4653.y), i32(_e4653.z), 0f);
                                    phi_32208_ = (_e4635 + (_e4659.y * _e4646));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4664 = phi_32208_;
                                    phi_25696_ = _e4664;
                                    phi_25695_ = (_e4637 + 1i);
                                }
                            }
                            let _e4668 = local_77;
                            let _e4670 = local_78;
                            phi_25698_ = mix(_e4668, _e4670, _e4627.x);
                        }
                        let _e4673 = phi_25698_;
                        let _e4675 = ((_e4554 & 2u) != 0u);
                        phi_13304_ = _e4675;
                        if _e4675 {
                            phi_13304_ = (_e4627.y > 0.0001f);
                        }
                        let _e4679 = phi_13304_;
                        phi_25702_ = _e4673;
                        if _e4679 {
                            phi_25700_ = 0f;
                            phi_25699_ = 0i;
                            loop {
                                let _e4681 = phi_25700_;
                                let _e4683 = phi_25699_;
                                local_79 = _e4681;
                                if (_e4683 < _e4516) {
                                    let _e4687 = f32((1i << bitcast<u32>(_e4683)));
                                    let _e4692 = (1f - smoothstep((_e4687 * 15f), (_e4687 * 30f), _e164));
                                    if (_e4692 <= 0f) {
                                        phi_32209_ = _e4681;
                                        continue;
                                    }
                                    let _e4695 = (_e4459 / vec2((0.5f * _e4687)));
                                    let _e4699 = vec3<f32>(_e4695.x, _e4695.y, f32(_e4683));
                                    let _e4705 = textureSampleLevel(global_8, global_7, vec2<f32>(_e4699.x, _e4699.y), i32(_e4699.z), 0f);
                                    phi_32209_ = (_e4681 + (_e4705.y * _e4692));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4710 = phi_32209_;
                                    phi_25700_ = _e4710;
                                    phi_25699_ = (_e4683 + 1i);
                                }
                            }
                            let _e4714 = local_79;
                            phi_25702_ = mix(_e4673, _e4714, _e4627.y);
                        }
                        let _e4717 = phi_25702_;
                        let _e4719 = ((_e4554 & 4u) != 0u);
                        phi_13322_ = _e4719;
                        if _e4719 {
                            phi_13322_ = (_e4627.z > 0.0001f);
                        }
                        let _e4723 = phi_13322_;
                        phi_25706_ = _e4717;
                        if _e4723 {
                            phi_25704_ = 0f;
                            phi_25703_ = 0i;
                            loop {
                                let _e4725 = phi_25704_;
                                let _e4727 = phi_25703_;
                                local_80 = _e4725;
                                if (_e4727 < _e4516) {
                                    let _e4731 = f32((1i << bitcast<u32>(_e4727)));
                                    let _e4736 = (1f - smoothstep((_e4731 * 15f), (_e4731 * 30f), _e164));
                                    if (_e4736 <= 0f) {
                                        phi_32210_ = _e4725;
                                        continue;
                                    }
                                    let _e4739 = (_e4459 / vec2((0.5f * _e4731)));
                                    let _e4743 = vec3<f32>(_e4739.x, _e4739.y, f32(_e4727));
                                    let _e4749 = textureSampleLevel(global_10, global_9, vec2<f32>(_e4743.x, _e4743.y), i32(_e4743.z), 0f);
                                    phi_32210_ = (_e4725 + (_e4749.y * _e4736));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4754 = phi_32210_;
                                    phi_25704_ = _e4754;
                                    phi_25703_ = (_e4727 + 1i);
                                }
                            }
                            let _e4758 = local_80;
                            phi_25706_ = mix(_e4717, _e4758, _e4627.z);
                        }
                        let _e4761 = phi_25706_;
                        let _e4763 = ((_e4554 & 8u) != 0u);
                        phi_13340_ = _e4763;
                        if _e4763 {
                            phi_13340_ = (_e4627.w > 0.0001f);
                        }
                        let _e4767 = phi_13340_;
                        phi_26486_ = _e4761;
                        if _e4767 {
                            phi_25708_ = 0f;
                            phi_25707_ = 0i;
                            loop {
                                let _e4769 = phi_25708_;
                                let _e4771 = phi_25707_;
                                local_81 = _e4769;
                                if (_e4771 < _e4516) {
                                    let _e4775 = f32((1i << bitcast<u32>(_e4771)));
                                    let _e4780 = (1f - smoothstep((_e4775 * 15f), (_e4775 * 30f), _e164));
                                    if (_e4780 <= 0f) {
                                        phi_32211_ = _e4769;
                                        continue;
                                    }
                                    let _e4783 = (_e4459 / vec2((0.5f * _e4775)));
                                    let _e4787 = vec3<f32>(_e4783.x, _e4783.y, f32(_e4771));
                                    let _e4793 = textureSampleLevel(global_12, global_11, vec2<f32>(_e4787.x, _e4787.y), i32(_e4787.z), 0f);
                                    phi_32211_ = (_e4769 + (_e4793.y * _e4780));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4798 = phi_32211_;
                                    phi_25708_ = _e4798;
                                    phi_25707_ = (_e4771 + 1i);
                                }
                            }
                            let _e4802 = local_81;
                            phi_26486_ = mix(_e4761, _e4802, _e4627.w);
                        }
                        let _e4805 = phi_26486_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e4808 = local_82;
                                if _e4808 {
                                    phi_25716_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e4811 = global.member_10[0u];
                                let _e4814 = min(i32((_e4811 + 0.5f)), 7i);
                                if (_e4814 <= 0i) {
                                    phi_25716_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e4816 = textureDimensions(global_2, 0i);
                                let _e4819 = f32(vec2<i32>(_e4816).x);
                                phi_25709_ = 0i;
                                loop {
                                    let _e4821 = phi_25709_;
                                    phi_25717_ = vec4<f32>();
                                    phi_25712_ = false;
                                    if (_e4821 < 7i) {
                                        if (_e4821 >= _e4814) {
                                            phi_25717_ = vec4<f32>();
                                            phi_25712_ = false;
                                            break;
                                        }
                                        let _e4826 = global.member_9[_e4821];
                                        let _e4832 = ((_e4459 - _e4826.xy) / vec2((_e4826.z * _e4819)));
                                        let _e4834 = all((_e4832 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_14317_ = _e4834;
                                        if _e4834 {
                                            phi_14317_ = all((_e4832 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e4838 = phi_14317_;
                                        if _e4838 {
                                            let _e4842 = vec3<f32>(_e4832.x, _e4832.y, f32(_e4821));
                                            let _e4848 = textureSampleLevel(global_4, global_3, vec2<f32>(_e4842.x, _e4842.y), i32(_e4842.z), 0f);
                                            phi_25717_ = clamp(_e4848, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_25712_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_25709_ = (_e4821 + 1i);
                                    }
                                }
                                let _e4852 = phi_25717_;
                                let _e4854 = phi_25712_;
                                phi_25716_ = _e4852;
                                if _e4854 {
                                    break;
                                }
                                let _e4855 = (_e4814 - 1i);
                                let _e4858 = global.member_9[_e4855];
                                let _e4865 = clamp(((_e4459 - _e4858.xy) / vec2((_e4858.z * _e4819))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e4869 = vec3<f32>(_e4865.x, _e4865.y, f32(_e4855));
                                let _e4875 = textureSampleLevel(global_4, global_3, vec2<f32>(_e4869.x, _e4869.y), i32(_e4869.z), 0f);
                                phi_25716_ = clamp(_e4875, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e4878 = phi_25716_;
                        let _e4880 = ((_e4554 & 16u) != 0u);
                        phi_13360_ = _e4880;
                        if _e4880 {
                            phi_13360_ = (_e4878.x > 0.0001f);
                        }
                        let _e4884 = phi_13360_;
                        phi_26495_ = _e4805;
                        if _e4884 {
                            phi_26493_ = 0f;
                            phi_26492_ = 0i;
                            loop {
                                let _e4886 = phi_26493_;
                                let _e4888 = phi_26492_;
                                local_83 = _e4886;
                                if (_e4888 < _e4516) {
                                    let _e4892 = f32((1i << bitcast<u32>(_e4888)));
                                    let _e4897 = (1f - smoothstep((_e4892 * 15f), (_e4892 * 30f), _e164));
                                    if (_e4897 <= 0f) {
                                        phi_32245_ = _e4886;
                                        continue;
                                    }
                                    let _e4900 = (_e4459 / vec2((0.5f * _e4892)));
                                    let _e4904 = vec3<f32>(_e4900.x, _e4900.y, f32(_e4888));
                                    let _e4910 = textureSampleLevel(global_14, global_13, vec2<f32>(_e4904.x, _e4904.y), i32(_e4904.z), 0f);
                                    phi_32245_ = (_e4886 + (_e4910.y * _e4897));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4915 = phi_32245_;
                                    phi_26493_ = _e4915;
                                    phi_26492_ = (_e4888 + 1i);
                                }
                            }
                            let _e4919 = local_83;
                            phi_26495_ = mix(_e4805, _e4919, _e4878.x);
                        }
                        let _e4922 = phi_26495_;
                        let _e4924 = ((_e4554 & 32u) != 0u);
                        phi_13378_ = _e4924;
                        if _e4924 {
                            phi_13378_ = (_e4878.y > 0.0001f);
                        }
                        let _e4928 = phi_13378_;
                        phi_26499_ = _e4922;
                        if _e4928 {
                            phi_26497_ = 0f;
                            phi_26496_ = 0i;
                            loop {
                                let _e4930 = phi_26497_;
                                let _e4932 = phi_26496_;
                                local_84 = _e4930;
                                if (_e4932 < _e4516) {
                                    let _e4936 = f32((1i << bitcast<u32>(_e4932)));
                                    let _e4941 = (1f - smoothstep((_e4936 * 15f), (_e4936 * 30f), _e164));
                                    if (_e4941 <= 0f) {
                                        phi_32246_ = _e4930;
                                        continue;
                                    }
                                    let _e4944 = (_e4459 / vec2((0.5f * _e4936)));
                                    let _e4948 = vec3<f32>(_e4944.x, _e4944.y, f32(_e4932));
                                    let _e4954 = textureSampleLevel(global_16, global_15, vec2<f32>(_e4948.x, _e4948.y), i32(_e4948.z), 0f);
                                    phi_32246_ = (_e4930 + (_e4954.y * _e4941));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e4959 = phi_32246_;
                                    phi_26497_ = _e4959;
                                    phi_26496_ = (_e4932 + 1i);
                                }
                            }
                            let _e4963 = local_84;
                            phi_26499_ = mix(_e4922, _e4963, _e4878.y);
                        }
                        let _e4966 = phi_26499_;
                        let _e4968 = ((_e4554 & 64u) != 0u);
                        phi_13396_ = _e4968;
                        if _e4968 {
                            phi_13396_ = (_e4878.z > 0.0001f);
                        }
                        let _e4972 = phi_13396_;
                        phi_26503_ = _e4966;
                        if _e4972 {
                            phi_26501_ = 0f;
                            phi_26500_ = 0i;
                            loop {
                                let _e4974 = phi_26501_;
                                let _e4976 = phi_26500_;
                                local_85 = _e4974;
                                if (_e4976 < _e4516) {
                                    let _e4980 = f32((1i << bitcast<u32>(_e4976)));
                                    let _e4985 = (1f - smoothstep((_e4980 * 15f), (_e4980 * 30f), _e164));
                                    if (_e4985 <= 0f) {
                                        phi_32247_ = _e4974;
                                        continue;
                                    }
                                    let _e4988 = (_e4459 / vec2((0.5f * _e4980)));
                                    let _e4992 = vec3<f32>(_e4988.x, _e4988.y, f32(_e4976));
                                    let _e4998 = textureSampleLevel(global_18, global_17, vec2<f32>(_e4992.x, _e4992.y), i32(_e4992.z), 0f);
                                    phi_32247_ = (_e4974 + (_e4998.y * _e4985));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e5003 = phi_32247_;
                                    phi_26501_ = _e5003;
                                    phi_26500_ = (_e4976 + 1i);
                                }
                            }
                            let _e5007 = local_85;
                            phi_26503_ = mix(_e4966, _e5007, _e4878.z);
                        }
                        let _e5010 = phi_26503_;
                        let _e5012 = ((_e4554 & 128u) != 0u);
                        phi_13414_ = _e5012;
                        if _e5012 {
                            phi_13414_ = (_e4878.w > 0.0001f);
                        }
                        let _e5016 = phi_13414_;
                        phi_26507_ = _e5010;
                        if _e5016 {
                            phi_26505_ = 0f;
                            phi_26504_ = 0i;
                            loop {
                                let _e5018 = phi_26505_;
                                let _e5020 = phi_26504_;
                                local_86 = _e5018;
                                if (_e5020 < _e4516) {
                                    let _e5024 = f32((1i << bitcast<u32>(_e5020)));
                                    let _e5029 = (1f - smoothstep((_e5024 * 15f), (_e5024 * 30f), _e164));
                                    if (_e5029 <= 0f) {
                                        phi_32248_ = _e5018;
                                        continue;
                                    }
                                    let _e5032 = (_e4459 / vec2((0.5f * _e5024)));
                                    let _e5036 = vec3<f32>(_e5032.x, _e5032.y, f32(_e5020));
                                    let _e5042 = textureSampleLevel(global_20, global_19, vec2<f32>(_e5036.x, _e5036.y), i32(_e5036.z), 0f);
                                    phi_32248_ = (_e5018 + (_e5042.y * _e5029));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e5047 = phi_32248_;
                                    phi_26505_ = _e5047;
                                    phi_26504_ = (_e5020 + 1i);
                                }
                            }
                            let _e5051 = local_86;
                            phi_26507_ = mix(_e5010, _e5051, _e4878.w);
                        }
                        let _e5054 = phi_26507_;
                        phi_26506_ = _e5054;
                    }
                    let _e5056 = phi_26506_;
                    let _e5059 = global.member_7[2u];
                    let _e5063 = global.member_7[3u];
                    phi_26687_ = (_e125 + clamp((_e5056 * _e5059), -(_e5063), _e5063));
                    break;
                }
            }
            let _e5068 = phi_26687_;
            let _e5069 = (_e3239 - vec2<f32>(0f, 0.35f));
            switch bitcast<i32>(0u) {
                default: {
                    let _e5072 = local_87;
                    let _e6994 = local_140;
                    phi_14963_ = _e6994;
                    if _e5072 {
                        let _e5075 = global.member_7[1u];
                        phi_14963_ = (_e5075 < 0.5f);
                    }
                    let _e5078 = phi_14963_;
                    if _e5078 {
                        let _e5081 = global.member_11[0u];
                        phi_28657_ = _e125;
                        phi_28656_ = 0i;
                        loop {
                            let _e5085 = phi_28657_;
                            let _e5087 = phi_28656_;
                            local_143 = _e5085;
                            if (_e5087 < i32(min(_e5081, 16f))) {
                                continue;
                            } else {
                                break;
                            }
                            continuing {
                                let _e5089 = (_e5087 * 2i);
                                let _e5092 = global.member_12[_e5089];
                                let _e5096 = global.member_12[(_e5089 + 1i)];
                                let _e5097 = _e5092.xy;
                                let _e5103 = max(_e5092.w, 0.001f);
                                let _e5104 = (6.2831855f / _e5103);
                                let _e5113 = global.member_2[0u];
                                phi_28657_ = (_e5085 + (_e5092.z * sin(((_e5104 * dot((_e5097 / vec2(max(length(_e5097), 0.0001f))), _e5069)) + (_e5113 * (_e5104 * (sqrt((1.56131f * _e5103)) * _e5096.y)))))));
                                phi_28656_ = (_e5087 + 1i);
                            }
                        }
                        let _e7067 = local_143;
                        phi_28658_ = _e7067;
                        break;
                    }
                    let _e5124 = global.member_7[1u];
                    let _e5126 = i32(min(_e5124, 16f));
                    phi_26689_ = 0f;
                    phi_26688_ = 0i;
                    loop {
                        let _e5128 = phi_26689_;
                        let _e5130 = phi_26688_;
                        local_88 = _e5128;
                        local_141 = _e5128;
                        local_142 = _e5128;
                        if (_e5130 < _e5126) {
                            let _e5134 = f32((1i << bitcast<u32>(_e5130)));
                            let _e5139 = (1f - smoothstep((_e5134 * 15f), (_e5134 * 30f), _e164));
                            if (_e5139 <= 0f) {
                                phi_32249_ = _e5128;
                                continue;
                            }
                            let _e5142 = (_e5069 / vec2((0.5f * _e5134)));
                            let _e5146 = vec3<f32>(_e5142.x, _e5142.y, f32(_e5130));
                            let _e5152 = textureSampleLevel(global_22, global_21, vec2<f32>(_e5146.x, _e5146.y), i32(_e5146.z), 0f);
                            phi_32249_ = (_e5128 + (_e5152.y * _e5139));
                            continue;
                        } else {
                            break;
                        }
                        continuing {
                            let _e5157 = phi_32249_;
                            phi_26689_ = _e5157;
                            phi_26688_ = (_e5130 + 1i);
                        }
                    }
                    let _e5161 = global.member_8[0u];
                    let _e5164 = u32((max(_e5161, 0f) + 0.5f));
                    let _e7064 = local_142;
                    phi_28477_ = _e7064;
                    if (_e5164 != 0u) {
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e5167 = (_e5164 == 0u);
                                local_93 = _e5167;
                                if _e5167 {
                                    phi_26697_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e5170 = global.member_10[0u];
                                let _e5173 = min(i32((_e5170 + 0.5f)), 7i);
                                if (_e5173 <= 0i) {
                                    phi_26697_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e5175 = textureDimensions(global_2, 0i);
                                let _e5178 = f32(vec2<i32>(_e5175).x);
                                phi_26690_ = 0i;
                                loop {
                                    let _e5180 = phi_26690_;
                                    phi_26698_ = vec4<f32>();
                                    phi_26693_ = false;
                                    if (_e5180 < 7i) {
                                        if (_e5180 >= _e5173) {
                                            phi_26698_ = vec4<f32>();
                                            phi_26693_ = false;
                                            break;
                                        }
                                        let _e5185 = global.member_9[_e5180];
                                        let _e5191 = ((_e5069 - _e5185.xy) / vec2((_e5185.z * _e5178)));
                                        let _e5193 = all((_e5191 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_15349_ = _e5193;
                                        if _e5193 {
                                            phi_15349_ = all((_e5191 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e5197 = phi_15349_;
                                        if _e5197 {
                                            let _e5201 = vec3<f32>(_e5191.x, _e5191.y, f32(_e5180));
                                            let _e5207 = textureSampleLevel(global_2, global_1, vec2<f32>(_e5201.x, _e5201.y), i32(_e5201.z), 0f);
                                            phi_26698_ = clamp(_e5207, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_26693_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_26690_ = (_e5180 + 1i);
                                    }
                                }
                                let _e5211 = phi_26698_;
                                let _e5213 = phi_26693_;
                                phi_26697_ = _e5211;
                                if _e5213 {
                                    break;
                                }
                                let _e5214 = (_e5173 - 1i);
                                let _e5217 = global.member_9[_e5214];
                                let _e5224 = clamp(((_e5069 - _e5217.xy) / vec2((_e5217.z * _e5178))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e5228 = vec3<f32>(_e5224.x, _e5224.y, f32(_e5214));
                                let _e5234 = textureSampleLevel(global_2, global_1, vec2<f32>(_e5228.x, _e5228.y), i32(_e5228.z), 0f);
                                phi_26697_ = clamp(_e5234, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e5237 = phi_26697_;
                        let _e5239 = ((_e5164 & 1u) != 0u);
                        phi_14985_ = _e5239;
                        if _e5239 {
                            phi_14985_ = (_e5237.x > 0.0001f);
                        }
                        let _e5243 = phi_14985_;
                        let _e7013 = local_141;
                        phi_27563_ = _e7013;
                        if _e5243 {
                            phi_27561_ = 0f;
                            phi_27560_ = 0i;
                            loop {
                                let _e5245 = phi_27561_;
                                let _e5247 = phi_27560_;
                                local_89 = _e5245;
                                if (_e5247 < _e5126) {
                                    let _e5251 = f32((1i << bitcast<u32>(_e5247)));
                                    let _e5256 = (1f - smoothstep((_e5251 * 15f), (_e5251 * 30f), _e164));
                                    if (_e5256 <= 0f) {
                                        phi_32251_ = _e5245;
                                        continue;
                                    }
                                    let _e5259 = (_e5069 / vec2((0.5f * _e5251)));
                                    let _e5263 = vec3<f32>(_e5259.x, _e5259.y, f32(_e5247));
                                    let _e5269 = textureSampleLevel(global_6, global_5, vec2<f32>(_e5263.x, _e5263.y), i32(_e5263.z), 0f);
                                    phi_32251_ = (_e5245 + (_e5269.y * _e5256));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e5274 = phi_32251_;
                                    phi_27561_ = _e5274;
                                    phi_27560_ = (_e5247 + 1i);
                                }
                            }
                            let _e5278 = local_88;
                            let _e5280 = local_89;
                            phi_27563_ = mix(_e5278, _e5280, _e5237.x);
                        }
                        let _e5283 = phi_27563_;
                        let _e5285 = ((_e5164 & 2u) != 0u);
                        phi_15003_ = _e5285;
                        if _e5285 {
                            phi_15003_ = (_e5237.y > 0.0001f);
                        }
                        let _e5289 = phi_15003_;
                        phi_27567_ = _e5283;
                        if _e5289 {
                            phi_27565_ = 0f;
                            phi_27564_ = 0i;
                            loop {
                                let _e5291 = phi_27565_;
                                let _e5293 = phi_27564_;
                                local_90 = _e5291;
                                if (_e5293 < _e5126) {
                                    let _e5297 = f32((1i << bitcast<u32>(_e5293)));
                                    let _e5302 = (1f - smoothstep((_e5297 * 15f), (_e5297 * 30f), _e164));
                                    if (_e5302 <= 0f) {
                                        phi_32252_ = _e5291;
                                        continue;
                                    }
                                    let _e5305 = (_e5069 / vec2((0.5f * _e5297)));
                                    let _e5309 = vec3<f32>(_e5305.x, _e5305.y, f32(_e5293));
                                    let _e5315 = textureSampleLevel(global_8, global_7, vec2<f32>(_e5309.x, _e5309.y), i32(_e5309.z), 0f);
                                    phi_32252_ = (_e5291 + (_e5315.y * _e5302));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e5320 = phi_32252_;
                                    phi_27565_ = _e5320;
                                    phi_27564_ = (_e5293 + 1i);
                                }
                            }
                            let _e5324 = local_90;
                            phi_27567_ = mix(_e5283, _e5324, _e5237.y);
                        }
                        let _e5327 = phi_27567_;
                        let _e5329 = ((_e5164 & 4u) != 0u);
                        phi_15021_ = _e5329;
                        if _e5329 {
                            phi_15021_ = (_e5237.z > 0.0001f);
                        }
                        let _e5333 = phi_15021_;
                        phi_27571_ = _e5327;
                        if _e5333 {
                            phi_27569_ = 0f;
                            phi_27568_ = 0i;
                            loop {
                                let _e5335 = phi_27569_;
                                let _e5337 = phi_27568_;
                                local_91 = _e5335;
                                if (_e5337 < _e5126) {
                                    let _e5341 = f32((1i << bitcast<u32>(_e5337)));
                                    let _e5346 = (1f - smoothstep((_e5341 * 15f), (_e5341 * 30f), _e164));
                                    if (_e5346 <= 0f) {
                                        phi_32253_ = _e5335;
                                        continue;
                                    }
                                    let _e5349 = (_e5069 / vec2((0.5f * _e5341)));
                                    let _e5353 = vec3<f32>(_e5349.x, _e5349.y, f32(_e5337));
                                    let _e5359 = textureSampleLevel(global_10, global_9, vec2<f32>(_e5353.x, _e5353.y), i32(_e5353.z), 0f);
                                    phi_32253_ = (_e5335 + (_e5359.y * _e5346));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e5364 = phi_32253_;
                                    phi_27569_ = _e5364;
                                    phi_27568_ = (_e5337 + 1i);
                                }
                            }
                            let _e5368 = local_91;
                            phi_27571_ = mix(_e5327, _e5368, _e5237.z);
                        }
                        let _e5371 = phi_27571_;
                        let _e5373 = ((_e5164 & 8u) != 0u);
                        phi_15039_ = _e5373;
                        if _e5373 {
                            phi_15039_ = (_e5237.w > 0.0001f);
                        }
                        let _e5377 = phi_15039_;
                        phi_28457_ = _e5371;
                        if _e5377 {
                            phi_27573_ = 0f;
                            phi_27572_ = 0i;
                            loop {
                                let _e5379 = phi_27573_;
                                let _e5381 = phi_27572_;
                                local_92 = _e5379;
                                if (_e5381 < _e5126) {
                                    let _e5385 = f32((1i << bitcast<u32>(_e5381)));
                                    let _e5390 = (1f - smoothstep((_e5385 * 15f), (_e5385 * 30f), _e164));
                                    if (_e5390 <= 0f) {
                                        phi_32254_ = _e5379;
                                        continue;
                                    }
                                    let _e5393 = (_e5069 / vec2((0.5f * _e5385)));
                                    let _e5397 = vec3<f32>(_e5393.x, _e5393.y, f32(_e5381));
                                    let _e5403 = textureSampleLevel(global_12, global_11, vec2<f32>(_e5397.x, _e5397.y), i32(_e5397.z), 0f);
                                    phi_32254_ = (_e5379 + (_e5403.y * _e5390));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e5408 = phi_32254_;
                                    phi_27573_ = _e5408;
                                    phi_27572_ = (_e5381 + 1i);
                                }
                            }
                            let _e5412 = local_92;
                            phi_28457_ = mix(_e5371, _e5412, _e5237.w);
                        }
                        let _e5415 = phi_28457_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e5418 = local_93;
                                if _e5418 {
                                    phi_27581_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e5421 = global.member_10[0u];
                                let _e5424 = min(i32((_e5421 + 0.5f)), 7i);
                                if (_e5424 <= 0i) {
                                    phi_27581_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e5426 = textureDimensions(global_2, 0i);
                                let _e5429 = f32(vec2<i32>(_e5426).x);
                                phi_27574_ = 0i;
                                loop {
                                    let _e5431 = phi_27574_;
                                    phi_27582_ = vec4<f32>();
                                    phi_27577_ = false;
                                    if (_e5431 < 7i) {
                                        if (_e5431 >= _e5424) {
                                            phi_27582_ = vec4<f32>();
                                            phi_27577_ = false;
                                            break;
                                        }
                                        let _e5436 = global.member_9[_e5431];
                                        let _e5442 = ((_e5069 - _e5436.xy) / vec2((_e5436.z * _e5429)));
                                        let _e5444 = all((_e5442 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_16016_ = _e5444;
                                        if _e5444 {
                                            phi_16016_ = all((_e5442 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e5448 = phi_16016_;
                                        if _e5448 {
                                            let _e5452 = vec3<f32>(_e5442.x, _e5442.y, f32(_e5431));
                                            let _e5458 = textureSampleLevel(global_4, global_3, vec2<f32>(_e5452.x, _e5452.y), i32(_e5452.z), 0f);
                                            phi_27582_ = clamp(_e5458, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_27577_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_27574_ = (_e5431 + 1i);
                                    }
                                }
                                let _e5462 = phi_27582_;
                                let _e5464 = phi_27577_;
                                phi_27581_ = _e5462;
                                if _e5464 {
                                    break;
                                }
                                let _e5465 = (_e5424 - 1i);
                                let _e5468 = global.member_9[_e5465];
                                let _e5475 = clamp(((_e5069 - _e5468.xy) / vec2((_e5468.z * _e5429))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e5479 = vec3<f32>(_e5475.x, _e5475.y, f32(_e5465));
                                let _e5485 = textureSampleLevel(global_4, global_3, vec2<f32>(_e5479.x, _e5479.y), i32(_e5479.z), 0f);
                                phi_27581_ = clamp(_e5485, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e5488 = phi_27581_;
                        let _e5490 = ((_e5164 & 16u) != 0u);
                        phi_15059_ = _e5490;
                        if _e5490 {
                            phi_15059_ = (_e5488.x > 0.0001f);
                        }
                        let _e5494 = phi_15059_;
                        phi_28466_ = _e5415;
                        if _e5494 {
                            phi_28464_ = 0f;
                            phi_28463_ = 0i;
                            loop {
                                let _e5496 = phi_28464_;
                                let _e5498 = phi_28463_;
                                local_94 = _e5496;
                                if (_e5498 < _e5126) {
                                    let _e5502 = f32((1i << bitcast<u32>(_e5498)));
                                    let _e5507 = (1f - smoothstep((_e5502 * 15f), (_e5502 * 30f), _e164));
                                    if (_e5507 <= 0f) {
                                        phi_32288_ = _e5496;
                                        continue;
                                    }
                                    let _e5510 = (_e5069 / vec2((0.5f * _e5502)));
                                    let _e5514 = vec3<f32>(_e5510.x, _e5510.y, f32(_e5498));
                                    let _e5520 = textureSampleLevel(global_14, global_13, vec2<f32>(_e5514.x, _e5514.y), i32(_e5514.z), 0f);
                                    phi_32288_ = (_e5496 + (_e5520.y * _e5507));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e5525 = phi_32288_;
                                    phi_28464_ = _e5525;
                                    phi_28463_ = (_e5498 + 1i);
                                }
                            }
                            let _e5529 = local_94;
                            phi_28466_ = mix(_e5415, _e5529, _e5488.x);
                        }
                        let _e5532 = phi_28466_;
                        let _e5534 = ((_e5164 & 32u) != 0u);
                        phi_15077_ = _e5534;
                        if _e5534 {
                            phi_15077_ = (_e5488.y > 0.0001f);
                        }
                        let _e5538 = phi_15077_;
                        phi_28470_ = _e5532;
                        if _e5538 {
                            phi_28468_ = 0f;
                            phi_28467_ = 0i;
                            loop {
                                let _e5540 = phi_28468_;
                                let _e5542 = phi_28467_;
                                local_95 = _e5540;
                                if (_e5542 < _e5126) {
                                    let _e5546 = f32((1i << bitcast<u32>(_e5542)));
                                    let _e5551 = (1f - smoothstep((_e5546 * 15f), (_e5546 * 30f), _e164));
                                    if (_e5551 <= 0f) {
                                        phi_32289_ = _e5540;
                                        continue;
                                    }
                                    let _e5554 = (_e5069 / vec2((0.5f * _e5546)));
                                    let _e5558 = vec3<f32>(_e5554.x, _e5554.y, f32(_e5542));
                                    let _e5564 = textureSampleLevel(global_16, global_15, vec2<f32>(_e5558.x, _e5558.y), i32(_e5558.z), 0f);
                                    phi_32289_ = (_e5540 + (_e5564.y * _e5551));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e5569 = phi_32289_;
                                    phi_28468_ = _e5569;
                                    phi_28467_ = (_e5542 + 1i);
                                }
                            }
                            let _e5573 = local_95;
                            phi_28470_ = mix(_e5532, _e5573, _e5488.y);
                        }
                        let _e5576 = phi_28470_;
                        let _e5578 = ((_e5164 & 64u) != 0u);
                        phi_15095_ = _e5578;
                        if _e5578 {
                            phi_15095_ = (_e5488.z > 0.0001f);
                        }
                        let _e5582 = phi_15095_;
                        phi_28474_ = _e5576;
                        if _e5582 {
                            phi_28472_ = 0f;
                            phi_28471_ = 0i;
                            loop {
                                let _e5584 = phi_28472_;
                                let _e5586 = phi_28471_;
                                local_96 = _e5584;
                                if (_e5586 < _e5126) {
                                    let _e5590 = f32((1i << bitcast<u32>(_e5586)));
                                    let _e5595 = (1f - smoothstep((_e5590 * 15f), (_e5590 * 30f), _e164));
                                    if (_e5595 <= 0f) {
                                        phi_32290_ = _e5584;
                                        continue;
                                    }
                                    let _e5598 = (_e5069 / vec2((0.5f * _e5590)));
                                    let _e5602 = vec3<f32>(_e5598.x, _e5598.y, f32(_e5586));
                                    let _e5608 = textureSampleLevel(global_18, global_17, vec2<f32>(_e5602.x, _e5602.y), i32(_e5602.z), 0f);
                                    phi_32290_ = (_e5584 + (_e5608.y * _e5595));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e5613 = phi_32290_;
                                    phi_28472_ = _e5613;
                                    phi_28471_ = (_e5586 + 1i);
                                }
                            }
                            let _e5617 = local_96;
                            phi_28474_ = mix(_e5576, _e5617, _e5488.z);
                        }
                        let _e5620 = phi_28474_;
                        let _e5622 = ((_e5164 & 128u) != 0u);
                        phi_15113_ = _e5622;
                        if _e5622 {
                            phi_15113_ = (_e5488.w > 0.0001f);
                        }
                        let _e5626 = phi_15113_;
                        phi_28478_ = _e5620;
                        if _e5626 {
                            phi_28476_ = 0f;
                            phi_28475_ = 0i;
                            loop {
                                let _e5628 = phi_28476_;
                                let _e5630 = phi_28475_;
                                local_97 = _e5628;
                                if (_e5630 < _e5126) {
                                    let _e5634 = f32((1i << bitcast<u32>(_e5630)));
                                    let _e5639 = (1f - smoothstep((_e5634 * 15f), (_e5634 * 30f), _e164));
                                    if (_e5639 <= 0f) {
                                        phi_32291_ = _e5628;
                                        continue;
                                    }
                                    let _e5642 = (_e5069 / vec2((0.5f * _e5634)));
                                    let _e5646 = vec3<f32>(_e5642.x, _e5642.y, f32(_e5630));
                                    let _e5652 = textureSampleLevel(global_20, global_19, vec2<f32>(_e5646.x, _e5646.y), i32(_e5646.z), 0f);
                                    phi_32291_ = (_e5628 + (_e5652.y * _e5639));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e5657 = phi_32291_;
                                    phi_28476_ = _e5657;
                                    phi_28475_ = (_e5630 + 1i);
                                }
                            }
                            let _e5661 = local_97;
                            phi_28478_ = mix(_e5620, _e5661, _e5488.w);
                        }
                        let _e5664 = phi_28478_;
                        phi_28477_ = _e5664;
                    }
                    let _e5666 = phi_28477_;
                    let _e5669 = global.member_7[2u];
                    let _e5673 = global.member_7[3u];
                    phi_28658_ = (_e125 + clamp((_e5666 * _e5669), -(_e5673), _e5673));
                    break;
                }
            }
            let _e5678 = phi_28658_;
            let _e5679 = (_e3239 + vec2<f32>(0f, 0.35f));
            switch bitcast<i32>(0u) {
                default: {
                    let _e5682 = local_98;
                    let _e7071 = local_144;
                    phi_16662_ = _e7071;
                    if _e5682 {
                        let _e5685 = global.member_7[1u];
                        phi_16662_ = (_e5685 < 0.5f);
                    }
                    let _e5688 = phi_16662_;
                    if _e5688 {
                        let _e5691 = global.member_11[0u];
                        phi_30840_ = _e125;
                        phi_30839_ = 0i;
                        loop {
                            let _e5695 = phi_30840_;
                            let _e5697 = phi_30839_;
                            local_147 = _e5695;
                            if (_e5697 < i32(min(_e5691, 16f))) {
                                continue;
                            } else {
                                break;
                            }
                            continuing {
                                let _e5699 = (_e5697 * 2i);
                                let _e5702 = global.member_12[_e5699];
                                let _e5706 = global.member_12[(_e5699 + 1i)];
                                let _e5707 = _e5702.xy;
                                let _e5713 = max(_e5702.w, 0.001f);
                                let _e5714 = (6.2831855f / _e5713);
                                let _e5723 = global.member_2[0u];
                                phi_30840_ = (_e5695 + (_e5702.z * sin(((_e5714 * dot((_e5707 / vec2(max(length(_e5707), 0.0001f))), _e5679)) + (_e5723 * (_e5714 * (sqrt((1.56131f * _e5713)) * _e5706.y)))))));
                                phi_30839_ = (_e5697 + 1i);
                            }
                        }
                        let _e7144 = local_147;
                        phi_30841_ = _e7144;
                        break;
                    }
                    let _e5734 = global.member_7[1u];
                    let _e5736 = i32(min(_e5734, 16f));
                    phi_28660_ = 0f;
                    phi_28659_ = 0i;
                    loop {
                        let _e5738 = phi_28660_;
                        let _e5740 = phi_28659_;
                        local_99 = _e5738;
                        local_145 = _e5738;
                        local_146 = _e5738;
                        if (_e5740 < _e5736) {
                            let _e5744 = f32((1i << bitcast<u32>(_e5740)));
                            let _e5749 = (1f - smoothstep((_e5744 * 15f), (_e5744 * 30f), _e164));
                            if (_e5749 <= 0f) {
                                phi_32292_ = _e5738;
                                continue;
                            }
                            let _e5752 = (_e5679 / vec2((0.5f * _e5744)));
                            let _e5756 = vec3<f32>(_e5752.x, _e5752.y, f32(_e5740));
                            let _e5762 = textureSampleLevel(global_22, global_21, vec2<f32>(_e5756.x, _e5756.y), i32(_e5756.z), 0f);
                            phi_32292_ = (_e5738 + (_e5762.y * _e5749));
                            continue;
                        } else {
                            break;
                        }
                        continuing {
                            let _e5767 = phi_32292_;
                            phi_28660_ = _e5767;
                            phi_28659_ = (_e5740 + 1i);
                        }
                    }
                    let _e5771 = global.member_8[0u];
                    let _e5774 = u32((max(_e5771, 0f) + 0.5f));
                    let _e7141 = local_146;
                    phi_30660_ = _e7141;
                    if (_e5774 != 0u) {
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e5777 = (_e5774 == 0u);
                                local_104 = _e5777;
                                if _e5777 {
                                    phi_28668_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e5780 = global.member_10[0u];
                                let _e5783 = min(i32((_e5780 + 0.5f)), 7i);
                                if (_e5783 <= 0i) {
                                    phi_28668_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e5785 = textureDimensions(global_2, 0i);
                                let _e5788 = f32(vec2<i32>(_e5785).x);
                                phi_28661_ = 0i;
                                loop {
                                    let _e5790 = phi_28661_;
                                    phi_28669_ = vec4<f32>();
                                    phi_28664_ = false;
                                    if (_e5790 < 7i) {
                                        if (_e5790 >= _e5783) {
                                            phi_28669_ = vec4<f32>();
                                            phi_28664_ = false;
                                            break;
                                        }
                                        let _e5795 = global.member_9[_e5790];
                                        let _e5801 = ((_e5679 - _e5795.xy) / vec2((_e5795.z * _e5788)));
                                        let _e5803 = all((_e5801 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_17048_ = _e5803;
                                        if _e5803 {
                                            phi_17048_ = all((_e5801 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e5807 = phi_17048_;
                                        if _e5807 {
                                            let _e5811 = vec3<f32>(_e5801.x, _e5801.y, f32(_e5790));
                                            let _e5817 = textureSampleLevel(global_2, global_1, vec2<f32>(_e5811.x, _e5811.y), i32(_e5811.z), 0f);
                                            phi_28669_ = clamp(_e5817, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_28664_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_28661_ = (_e5790 + 1i);
                                    }
                                }
                                let _e5821 = phi_28669_;
                                let _e5823 = phi_28664_;
                                phi_28668_ = _e5821;
                                if _e5823 {
                                    break;
                                }
                                let _e5824 = (_e5783 - 1i);
                                let _e5827 = global.member_9[_e5824];
                                let _e5834 = clamp(((_e5679 - _e5827.xy) / vec2((_e5827.z * _e5788))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e5838 = vec3<f32>(_e5834.x, _e5834.y, f32(_e5824));
                                let _e5844 = textureSampleLevel(global_2, global_1, vec2<f32>(_e5838.x, _e5838.y), i32(_e5838.z), 0f);
                                phi_28668_ = clamp(_e5844, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e5847 = phi_28668_;
                        let _e5849 = ((_e5774 & 1u) != 0u);
                        phi_16684_ = _e5849;
                        if _e5849 {
                            phi_16684_ = (_e5847.x > 0.0001f);
                        }
                        let _e5853 = phi_16684_;
                        let _e7090 = local_145;
                        phi_29640_ = _e7090;
                        if _e5853 {
                            phi_29638_ = 0f;
                            phi_29637_ = 0i;
                            loop {
                                let _e5855 = phi_29638_;
                                let _e5857 = phi_29637_;
                                local_100 = _e5855;
                                if (_e5857 < _e5736) {
                                    let _e5861 = f32((1i << bitcast<u32>(_e5857)));
                                    let _e5866 = (1f - smoothstep((_e5861 * 15f), (_e5861 * 30f), _e164));
                                    if (_e5866 <= 0f) {
                                        phi_32294_ = _e5855;
                                        continue;
                                    }
                                    let _e5869 = (_e5679 / vec2((0.5f * _e5861)));
                                    let _e5873 = vec3<f32>(_e5869.x, _e5869.y, f32(_e5857));
                                    let _e5879 = textureSampleLevel(global_6, global_5, vec2<f32>(_e5873.x, _e5873.y), i32(_e5873.z), 0f);
                                    phi_32294_ = (_e5855 + (_e5879.y * _e5866));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e5884 = phi_32294_;
                                    phi_29638_ = _e5884;
                                    phi_29637_ = (_e5857 + 1i);
                                }
                            }
                            let _e5888 = local_99;
                            let _e5890 = local_100;
                            phi_29640_ = mix(_e5888, _e5890, _e5847.x);
                        }
                        let _e5893 = phi_29640_;
                        let _e5895 = ((_e5774 & 2u) != 0u);
                        phi_16702_ = _e5895;
                        if _e5895 {
                            phi_16702_ = (_e5847.y > 0.0001f);
                        }
                        let _e5899 = phi_16702_;
                        phi_29644_ = _e5893;
                        if _e5899 {
                            phi_29642_ = 0f;
                            phi_29641_ = 0i;
                            loop {
                                let _e5901 = phi_29642_;
                                let _e5903 = phi_29641_;
                                local_101 = _e5901;
                                if (_e5903 < _e5736) {
                                    let _e5907 = f32((1i << bitcast<u32>(_e5903)));
                                    let _e5912 = (1f - smoothstep((_e5907 * 15f), (_e5907 * 30f), _e164));
                                    if (_e5912 <= 0f) {
                                        phi_32295_ = _e5901;
                                        continue;
                                    }
                                    let _e5915 = (_e5679 / vec2((0.5f * _e5907)));
                                    let _e5919 = vec3<f32>(_e5915.x, _e5915.y, f32(_e5903));
                                    let _e5925 = textureSampleLevel(global_8, global_7, vec2<f32>(_e5919.x, _e5919.y), i32(_e5919.z), 0f);
                                    phi_32295_ = (_e5901 + (_e5925.y * _e5912));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e5930 = phi_32295_;
                                    phi_29642_ = _e5930;
                                    phi_29641_ = (_e5903 + 1i);
                                }
                            }
                            let _e5934 = local_101;
                            phi_29644_ = mix(_e5893, _e5934, _e5847.y);
                        }
                        let _e5937 = phi_29644_;
                        let _e5939 = ((_e5774 & 4u) != 0u);
                        phi_16720_ = _e5939;
                        if _e5939 {
                            phi_16720_ = (_e5847.z > 0.0001f);
                        }
                        let _e5943 = phi_16720_;
                        phi_29648_ = _e5937;
                        if _e5943 {
                            phi_29646_ = 0f;
                            phi_29645_ = 0i;
                            loop {
                                let _e5945 = phi_29646_;
                                let _e5947 = phi_29645_;
                                local_102 = _e5945;
                                if (_e5947 < _e5736) {
                                    let _e5951 = f32((1i << bitcast<u32>(_e5947)));
                                    let _e5956 = (1f - smoothstep((_e5951 * 15f), (_e5951 * 30f), _e164));
                                    if (_e5956 <= 0f) {
                                        phi_32296_ = _e5945;
                                        continue;
                                    }
                                    let _e5959 = (_e5679 / vec2((0.5f * _e5951)));
                                    let _e5963 = vec3<f32>(_e5959.x, _e5959.y, f32(_e5947));
                                    let _e5969 = textureSampleLevel(global_10, global_9, vec2<f32>(_e5963.x, _e5963.y), i32(_e5963.z), 0f);
                                    phi_32296_ = (_e5945 + (_e5969.y * _e5956));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e5974 = phi_32296_;
                                    phi_29646_ = _e5974;
                                    phi_29645_ = (_e5947 + 1i);
                                }
                            }
                            let _e5978 = local_102;
                            phi_29648_ = mix(_e5937, _e5978, _e5847.z);
                        }
                        let _e5981 = phi_29648_;
                        let _e5983 = ((_e5774 & 8u) != 0u);
                        phi_16738_ = _e5983;
                        if _e5983 {
                            phi_16738_ = (_e5847.w > 0.0001f);
                        }
                        let _e5987 = phi_16738_;
                        phi_30640_ = _e5981;
                        if _e5987 {
                            phi_29650_ = 0f;
                            phi_29649_ = 0i;
                            loop {
                                let _e5989 = phi_29650_;
                                let _e5991 = phi_29649_;
                                local_103 = _e5989;
                                if (_e5991 < _e5736) {
                                    let _e5995 = f32((1i << bitcast<u32>(_e5991)));
                                    let _e6000 = (1f - smoothstep((_e5995 * 15f), (_e5995 * 30f), _e164));
                                    if (_e6000 <= 0f) {
                                        phi_32297_ = _e5989;
                                        continue;
                                    }
                                    let _e6003 = (_e5679 / vec2((0.5f * _e5995)));
                                    let _e6007 = vec3<f32>(_e6003.x, _e6003.y, f32(_e5991));
                                    let _e6013 = textureSampleLevel(global_12, global_11, vec2<f32>(_e6007.x, _e6007.y), i32(_e6007.z), 0f);
                                    phi_32297_ = (_e5989 + (_e6013.y * _e6000));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e6018 = phi_32297_;
                                    phi_29650_ = _e6018;
                                    phi_29649_ = (_e5991 + 1i);
                                }
                            }
                            let _e6022 = local_103;
                            phi_30640_ = mix(_e5981, _e6022, _e5847.w);
                        }
                        let _e6025 = phi_30640_;
                        switch bitcast<i32>(0u) {
                            default: {
                                let _e6028 = local_104;
                                if _e6028 {
                                    phi_29658_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e6031 = global.member_10[0u];
                                let _e6034 = min(i32((_e6031 + 0.5f)), 7i);
                                if (_e6034 <= 0i) {
                                    phi_29658_ = vec4<f32>(0f, 0f, 0f, 0f);
                                    break;
                                }
                                let _e6036 = textureDimensions(global_2, 0i);
                                let _e6039 = f32(vec2<i32>(_e6036).x);
                                phi_29651_ = 0i;
                                loop {
                                    let _e6041 = phi_29651_;
                                    phi_29659_ = vec4<f32>();
                                    phi_29654_ = false;
                                    if (_e6041 < 7i) {
                                        if (_e6041 >= _e6034) {
                                            phi_29659_ = vec4<f32>();
                                            phi_29654_ = false;
                                            break;
                                        }
                                        let _e6046 = global.member_9[_e6041];
                                        let _e6052 = ((_e5679 - _e6046.xy) / vec2((_e6046.z * _e6039)));
                                        let _e6054 = all((_e6052 >= vec2<f32>(0.04f, 0.04f)));
                                        phi_17715_ = _e6054;
                                        if _e6054 {
                                            phi_17715_ = all((_e6052 <= vec2<f32>(0.96f, 0.96f)));
                                        }
                                        let _e6058 = phi_17715_;
                                        if _e6058 {
                                            let _e6062 = vec3<f32>(_e6052.x, _e6052.y, f32(_e6041));
                                            let _e6068 = textureSampleLevel(global_4, global_3, vec2<f32>(_e6062.x, _e6062.y), i32(_e6062.z), 0f);
                                            phi_29659_ = clamp(_e6068, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                            phi_29654_ = true;
                                            break;
                                        }
                                        continue;
                                    } else {
                                        break;
                                    }
                                    continuing {
                                        phi_29651_ = (_e6041 + 1i);
                                    }
                                }
                                let _e6072 = phi_29659_;
                                let _e6074 = phi_29654_;
                                phi_29658_ = _e6072;
                                if _e6074 {
                                    break;
                                }
                                let _e6075 = (_e6034 - 1i);
                                let _e6078 = global.member_9[_e6075];
                                let _e6085 = clamp(((_e5679 - _e6078.xy) / vec2((_e6078.z * _e6039))), vec2<f32>(0f, 0f), vec2<f32>(1f, 1f));
                                let _e6089 = vec3<f32>(_e6085.x, _e6085.y, f32(_e6075));
                                let _e6095 = textureSampleLevel(global_4, global_3, vec2<f32>(_e6089.x, _e6089.y), i32(_e6089.z), 0f);
                                phi_29658_ = clamp(_e6095, vec4<f32>(0f, 0f, 0f, 0f), vec4<f32>(1f, 1f, 1f, 1f));
                                break;
                            }
                        }
                        let _e6098 = phi_29658_;
                        let _e6100 = ((_e5774 & 16u) != 0u);
                        phi_16758_ = _e6100;
                        if _e6100 {
                            phi_16758_ = (_e6098.x > 0.0001f);
                        }
                        let _e6104 = phi_16758_;
                        phi_30649_ = _e6025;
                        if _e6104 {
                            phi_30647_ = 0f;
                            phi_30646_ = 0i;
                            loop {
                                let _e6106 = phi_30647_;
                                let _e6108 = phi_30646_;
                                local_105 = _e6106;
                                if (_e6108 < _e5736) {
                                    let _e6112 = f32((1i << bitcast<u32>(_e6108)));
                                    let _e6117 = (1f - smoothstep((_e6112 * 15f), (_e6112 * 30f), _e164));
                                    if (_e6117 <= 0f) {
                                        phi_32331_ = _e6106;
                                        continue;
                                    }
                                    let _e6120 = (_e5679 / vec2((0.5f * _e6112)));
                                    let _e6124 = vec3<f32>(_e6120.x, _e6120.y, f32(_e6108));
                                    let _e6130 = textureSampleLevel(global_14, global_13, vec2<f32>(_e6124.x, _e6124.y), i32(_e6124.z), 0f);
                                    phi_32331_ = (_e6106 + (_e6130.y * _e6117));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e6135 = phi_32331_;
                                    phi_30647_ = _e6135;
                                    phi_30646_ = (_e6108 + 1i);
                                }
                            }
                            let _e6139 = local_105;
                            phi_30649_ = mix(_e6025, _e6139, _e6098.x);
                        }
                        let _e6142 = phi_30649_;
                        let _e6144 = ((_e5774 & 32u) != 0u);
                        phi_16776_ = _e6144;
                        if _e6144 {
                            phi_16776_ = (_e6098.y > 0.0001f);
                        }
                        let _e6148 = phi_16776_;
                        phi_30653_ = _e6142;
                        if _e6148 {
                            phi_30651_ = 0f;
                            phi_30650_ = 0i;
                            loop {
                                let _e6150 = phi_30651_;
                                let _e6152 = phi_30650_;
                                local_106 = _e6150;
                                if (_e6152 < _e5736) {
                                    let _e6156 = f32((1i << bitcast<u32>(_e6152)));
                                    let _e6161 = (1f - smoothstep((_e6156 * 15f), (_e6156 * 30f), _e164));
                                    if (_e6161 <= 0f) {
                                        phi_32332_ = _e6150;
                                        continue;
                                    }
                                    let _e6164 = (_e5679 / vec2((0.5f * _e6156)));
                                    let _e6168 = vec3<f32>(_e6164.x, _e6164.y, f32(_e6152));
                                    let _e6174 = textureSampleLevel(global_16, global_15, vec2<f32>(_e6168.x, _e6168.y), i32(_e6168.z), 0f);
                                    phi_32332_ = (_e6150 + (_e6174.y * _e6161));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e6179 = phi_32332_;
                                    phi_30651_ = _e6179;
                                    phi_30650_ = (_e6152 + 1i);
                                }
                            }
                            let _e6183 = local_106;
                            phi_30653_ = mix(_e6142, _e6183, _e6098.y);
                        }
                        let _e6186 = phi_30653_;
                        let _e6188 = ((_e5774 & 64u) != 0u);
                        phi_16794_ = _e6188;
                        if _e6188 {
                            phi_16794_ = (_e6098.z > 0.0001f);
                        }
                        let _e6192 = phi_16794_;
                        phi_30657_ = _e6186;
                        if _e6192 {
                            phi_30655_ = 0f;
                            phi_30654_ = 0i;
                            loop {
                                let _e6194 = phi_30655_;
                                let _e6196 = phi_30654_;
                                local_107 = _e6194;
                                if (_e6196 < _e5736) {
                                    let _e6200 = f32((1i << bitcast<u32>(_e6196)));
                                    let _e6205 = (1f - smoothstep((_e6200 * 15f), (_e6200 * 30f), _e164));
                                    if (_e6205 <= 0f) {
                                        phi_32333_ = _e6194;
                                        continue;
                                    }
                                    let _e6208 = (_e5679 / vec2((0.5f * _e6200)));
                                    let _e6212 = vec3<f32>(_e6208.x, _e6208.y, f32(_e6196));
                                    let _e6218 = textureSampleLevel(global_18, global_17, vec2<f32>(_e6212.x, _e6212.y), i32(_e6212.z), 0f);
                                    phi_32333_ = (_e6194 + (_e6218.y * _e6205));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e6223 = phi_32333_;
                                    phi_30655_ = _e6223;
                                    phi_30654_ = (_e6196 + 1i);
                                }
                            }
                            let _e6227 = local_107;
                            phi_30657_ = mix(_e6186, _e6227, _e6098.z);
                        }
                        let _e6230 = phi_30657_;
                        let _e6232 = ((_e5774 & 128u) != 0u);
                        phi_16812_ = _e6232;
                        if _e6232 {
                            phi_16812_ = (_e6098.w > 0.0001f);
                        }
                        let _e6236 = phi_16812_;
                        phi_30661_ = _e6230;
                        if _e6236 {
                            phi_30659_ = 0f;
                            phi_30658_ = 0i;
                            loop {
                                let _e6238 = phi_30659_;
                                let _e6240 = phi_30658_;
                                local_108 = _e6238;
                                if (_e6240 < _e5736) {
                                    let _e6244 = f32((1i << bitcast<u32>(_e6240)));
                                    let _e6249 = (1f - smoothstep((_e6244 * 15f), (_e6244 * 30f), _e164));
                                    if (_e6249 <= 0f) {
                                        phi_32334_ = _e6238;
                                        continue;
                                    }
                                    let _e6252 = (_e5679 / vec2((0.5f * _e6244)));
                                    let _e6256 = vec3<f32>(_e6252.x, _e6252.y, f32(_e6240));
                                    let _e6262 = textureSampleLevel(global_20, global_19, vec2<f32>(_e6256.x, _e6256.y), i32(_e6256.z), 0f);
                                    phi_32334_ = (_e6238 + (_e6262.y * _e6249));
                                    continue;
                                } else {
                                    break;
                                }
                                continuing {
                                    let _e6267 = phi_32334_;
                                    phi_30659_ = _e6267;
                                    phi_30658_ = (_e6240 + 1i);
                                }
                            }
                            let _e6271 = local_108;
                            phi_30661_ = mix(_e6230, _e6271, _e6098.w);
                        }
                        let _e6274 = phi_30661_;
                        phi_30660_ = _e6274;
                    }
                    let _e6276 = phi_30660_;
                    let _e6279 = global.member_7[2u];
                    let _e6283 = global.member_7[3u];
                    phi_30841_ = (_e125 + clamp((_e6276 * _e6279), -(_e6283), _e6283));
                    break;
                }
            }
            let _e6288 = phi_30841_;
            let _e6295 = smoothstep(0.02f, 0.18f, reflect(_e3228, normalize(vec3<f32>((_e4458 - _e5068), 0.7f, (_e5678 - _e6288)))).y);
            let _e6296 = (_e119.y - _e3848);
            let _e6300 = global.member_6[1u];
            let _e6309 = global.member_6[2u];
            let _e6314 = (((1f - smoothstep(0f, max(max(_e6300, 0f), 0.001f), _e6296)) * exp((-(max(_e6296, 0f)) * max(_e6309, 0.01f)))) * smoothstep(0.08f, 0.24f, _e6296));
            let _e6316 = smoothstep(0.02f, 0.16f, _e168.y);
            phi_31904_ = _e100;
            if ((((_e6314 > 0.001f) && (_e159 > 0.001f)) && (_e6295 > 0.001f)) && (_e6316 > 0.001f)) {
                let _e6326 = global.member_5[0u];
                let _e6328 = (1f / max(_e6326, 0.001f));
                let _e6331 = global.member_2[0u];
                let _e6338 = textureSampleLevel(global_30, global_29, ((_e3239 * _e6328) + vec2<f32>(((0.044f * _e6331) + 17.16f), (-0.169f * _e6331))), 0f);
                let _e6346 = textureSampleLevel(global_30, global_29, (((_e3239 * 1.37f) * _e6328) + vec2<f32>((0.248f * _e6331), (0.117f * _e6331))), 0f);
                let _e6351 = global.member_5[1u];
                let _e6355 = global.member_5[2u];
                let _e6362 = global.member_6[0u];
                phi_31904_ = (_e100 * max(vec3<f32>(0f, 0f, 0f), vec3((1f + ((((_e6338.z * _e6346.z) - _e6351) * _e6355) * ((((_e6314 * _e159) * _e6295) * _e6316) * max(_e6362, 0f)))))));
            }
            let _e6371 = phi_31904_;
            global_28 = vec4<f32>(_e6371.x, _e6371.y, _e6371.z, 1f);
            break;
        }
    }
    return;
}

@fragment
fn main(@location(0) param: vec2<f32>) -> @location(0) vec4<f32> {
    global_25 = param;
    function_();
    let _e3 = global_28;
    return _e3;
}
