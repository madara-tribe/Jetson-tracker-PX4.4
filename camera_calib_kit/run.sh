# チェッカーボード画像を撮る — capture_calib_images.py
python3 capture_calib_images.py \
  --device /dev/video2 \
  --cols 9 --rows 6 --square-size-mm 25 \
  --out captures_ir \
  --auto --auto-interval 2.0

#--device はIRカメラのデバイスパス(v4l2-ctl --list-devices で確認)
#--cols/--rows は内側コーナー数(10×7マスの市松模様なら 9×6)
#--auto を付けるとコーナー検出できた瞬間から一定間隔で自動保存、外すと s キーで手動保存
#ボードを画面の四隅・中央・斜め・近距離/遠距離でまんべんなく動かしながら15〜20枚撮る(dayカメラの実績: 20枚、RMS 0.27px)
#IRカメラでコントラストが出にくい場合は --gray-only や周囲のIR照明を強めることを試してください

## チェッカーボード画像を撮る — capture_calib_images.py
python3 capture_calib_images.py \
  --device /dev/video2 \
  --cols 9 --rows 6 --square-size-mm 25 \
  --out captures_ir \
  --auto --auto-interval 2.0

#--device はIRカメラのデバイスパス(v4l2-ctl --list-devices で確認)
#--cols/--rows は内側コーナー数(10×7マスの市松模様なら 9×6)
#--auto を付けるとコーナー検出できた瞬間から一定間隔で自動保存、外すと s キーで手動保存
#ボードを画面の四隅・中央・斜め・近距離/遠距離でまんべんなく動かしながら15〜20枚撮る(dayカメラの実績: 20枚、RMS 0.27px)
#IRカメラでコントラストが出にくい場合は --gray-only や周囲のIR照明を強めることを試してください
