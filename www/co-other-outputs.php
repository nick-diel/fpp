<script>
/////////////////////////////////////////////////////////////////////////////
//Base Function

class CO-OtherBase {
    constructor() {
        this.typeName = "Base-Type";
        this.displayName = "Base Type";
        var maxChannels = 510;
    }
}

function CO-OtherBase() {
    this.typeName = "Base-Type";
    var maxChannels = 510;
    
    function PopulateHTMLRow(config) {
        var results = "Base-Type";
        return results;
    }

    function AddNewRow() {
        var config = {};
        return PopulateHTMLRow(config);
    }

    function GetOutputConfig(result, cell) {
        return result;
    }

    function GetMaxChannels() {
        return maxChannels;
    }
}

/////////////////////////////////////////////////////////////////////////////
// SPI Devices (/dev/spidev*

function SPI-WS2801Device() {
    var device = new CO-OtherBase();
    device.typeName = "SPI-WS2801";
    device.maxChannels = 1530;

    var SPIDevices = new Array();
<?
	foreach(scandir("/dev/") as $fileName)
	{
		if (preg_match("/^spidev[0-9]/", $fileName)) {
			echo "SPIDevices['$fileName'] = '$fileName';\n";
		}
	}
?>

    device.PopulateHTMLRow = function(config) {
        var result = "";

        result += DeviceSelect(SPIDevices, config.device);
        result += " PI36: <input type=checkbox class='pi36'";
        if (config.pi36)
            result += " checked='checked'";

        result += ">";

        return result;
    }

    device.AddNewRow() {
        var config = {};

        config.device = "";
        config.pi36 = 0;

        return device.PopulateHTMLRow(config);
    }

    device.GetOutputConfig = function(result, cell) {
        $cell = $(cell);
        var value = $cell.find("select.device").val();

        if (value == "")
            return "";

        var pi36 = 0;

        if ($cell.find("input.pi36").is(":checked"))
            pi36 = 1;

        result.device = value;
        result.pi36 = parseInt(pi36);

        return result;
        }

    return device;
}


</script>