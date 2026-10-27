# 標準 Zigbee 属性の変換

Zigbee Cluster Library の Temperature Measurement (0x0402) と Relative Humidity Measurement (0x0405) の MeasuredValue (0x0000) を対象とする。Tuya 独自形式には適用しない。

- 温度は符号付き 16 ビット整数で、100 で割ると摂氏温度。範囲は -273.15〜327.67 °C。0x8000 は無効で、-273.15 °C 未満も範囲外。
- 湿度は符号なし 16 ビット整数で、100 で割ると相対湿度のパーセント。範囲は 0〜100%。0xffff は無効で、100% を超える値も範囲外。
- 無効または範囲外の場合は `false` を返し、出力値を変更しない。呼び出し側は未取得値を区別する。

仕様: [Zigbee Cluster Library Specification, revision 6, 4.4 and 4.7](https://zigbeealliance.org/wp-content/uploads/2019/12/07-5123-06-zigbee-cluster-library-specification.pdf)、[NXP ZigBee Cluster Library User Guide, chapter 24](https://www.nxp.com/docs/en/user-guide/JN-UG-3077.pdf)。

ホストでのテスト例（C コンパイラーが必要）:

```sh
gcc -std=c11 -Wall -Wextra -Werror measurement/measurement.c measurement/test_measurement.c -lm -o measurement-test
./measurement-test
```
