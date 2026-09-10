1.需要vendor/qcom/opensource/vibrator/aidl/Android.bp中添加
visibility: [
        ":__subpackages__",
	"//vendor/qcom/aac/interface/richtap/vibrator:__subpackages__",
    ],
2. 如果修改了文件路径，需要在vendor/qcom/aac目录下修改Android.bp的path




m vendor.aac.hardware.richtap.vibrator-update-api
m vendor.aac.hardware.richtap.vibrator-freeze-api
