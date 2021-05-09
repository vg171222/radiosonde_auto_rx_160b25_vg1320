#!/usr/bin/env python
#
#   radiosonde_auto_rx - Log File Utilities
#
#   Copyright (C) 2021  Mark Jessop <vk5qi@rfhead.net>
#   Released under GNU GPL v3 or later
#
import autorx
import datetime
import glob
import logging
import os.path
import time

import numpy as np

from dateutil.parser import parse
from autorx.utils import (
    short_type_lookup,
    readable_timedelta,
    strip_sonde_serial,
    position_info,
)
from autorx.geometry import GenericTrack, getDensity


def log_filename_to_stats(filename):
    """ Attempt to extract information about a log file from a supplied filename """
    # Example log file name: 20210430-235413_IMET-89F2720A_IMET_401999_sonde.log
    # ./log/20200320-063233_R2230624_RS41_402500_sonde.log

    _basename = os.path.basename(filename)

    _now_dt = datetime.datetime.now(datetime.timezone.utc)

    try:
        _fields = _basename.split("_")

        # First field is the date/time the sonde was first received.
        _date_str = _fields[0] + "Z"
        _date_dt = parse(_date_str)

        # Calculate age
        _age_td = _now_dt - _date_dt
        _time_delta = readable_timedelta(_age_td)

        # Re-format date
        _date_str2 = _date_dt.strftime("%Y-%m-%dT%H:%M:%SZ")

        # Second field is the serial number, which may include a sonde type prefix.
        _serial = strip_sonde_serial(_fields[1])

        # Third field is the sonde type, in 'shortform'
        _type = _fields[2]
        _type_str = short_type_lookup(_type)

        # Fourth field is the sonde frequency in kHz
        _freq = float(_fields[3]) / 1e3

        return {
            "datetime": _date_str2,
            "age": _time_delta,
            "serial": _serial,
            "type": _type_str,
            "freq": _freq,
        }

    except Exception as e:
        logging.exception(f"Could not parse filename {_basename}", e)
        return None


def list_log_files():
    """ Look for all sonde log files within the logging directory """

    # Output list, which will contain one object per log file, ordered by time
    _output = []

    # Search for file matching the expected log file name
    _log_mask = os.path.join(autorx.logging_path, "*_sonde.log")
    _log_files = glob.glob(_log_mask)

    # Sort alphanumerically, which will result in the entries being date ordered
    _log_files.sort()
    # Flip array so newest is first.
    _log_files.reverse()

    for _file in _log_files:
        _entry = log_filename_to_stats(_file)
        if _entry:
            _output.append(_entry)

    return _output


def read_log_file(filename, skewt_decimation=10):
    """ Read in a log file """
    logging.debug(f"Attempting to read file: {filename}")

    # Open the file and get the header line
    _file = open(filename, "r")
    _header = _file.readline()

    # Initially assume a new style log file (> ~1.4.0)
    # timestamp,serial,frame,lat,lon,alt,vel_v,vel_h,heading,temp,humidity,pressure,type,freq_mhz,snr,f_error_hz,sats,batt_v,burst_timer,aux_data
    fields = {
        "datetime": "f0",
        "serial": "f1",
        "frame": "f2",
        "latitude": "f3",
        "longitude": "f4",
        "altitude": "f5",
        "vel_v": "f6",
        "vel_h": "f7",
        "heading": "f8",
        "temp": "f9",
        "humidity": "f10",
        "pressure": "f11",
        "type": "f12",
        "frequency": "f13",
        "snr": "f14",
        "sats": "f16",
        "batt": "f17",
    }

    if "other" in _header:
        # Older style log file
        # timestamp,serial,frame,lat,lon,alt,vel_v,vel_h,heading,temp,humidity,type,freq,other
        # 2020-06-06T00:58:09.001Z,R3670268,7685,-31.21523,137.68126,33752.4,5.9,2.1,44.5,-273.0,-1.0,RS41,401.501,SNR 5.4,FERROR -187,SATS 9,BATT 2.7
        fields = {
            "datetime": "f0",
            "serial": "f1",
            "frame": "f2",
            "latitude": "f3",
            "longitude": "f4",
            "altitude": "f5",
            "vel_v": "f6",
            "vel_h": "f7",
            "heading": "f8",
            "temp": "f9",
            "humidity": "f10",
            "type": "f11",
            "frequency": "f12",
        }
        # Only use a subset of the columns, as the number of colums can vary in this old format
        _data = np.genfromtxt(
            _file,
            dtype=None,
            encoding="ascii",
            delimiter=",",
            usecols=(0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12),
        )

    else:
        # Grab everything
        _data = np.genfromtxt(_file, dtype=None, encoding="ascii", delimiter=",")

    _file.close()

    # Now we need to rearrange some data for easier use in the client
    _output = {"serial": strip_sonde_serial(_data[fields["serial"]][0])}

    # Path to display on the map
    _output["path"] = np.column_stack(
        (
            _data[fields["latitude"]],
            _data[fields["longitude"]],
            _data[fields["altitude"]],
        )
    ).tolist()
    _output["first"] = _output["path"][0]
    _output["first_time"] = _data[fields["datetime"]][0]
    _output["last"] = _output["path"][-1]
    _output["last_time"] = _data[fields["datetime"]][-1]
    _burst_idx = np.argmax(_data[fields["altitude"]])
    _output["burst"] = _output["path"][_burst_idx]
    _output["burst_time"] = _data[fields["datetime"]][_burst_idx]

    # TODO: Calculate data necessary for Skew-T plots
    if "pressure" in fields:
        _press = _data[fields["pressure"]]
    else:
        _press = None

    _output["skewt"] = calculate_skewt_data(
        _data[fields["datetime"]],
        _data[fields["latitude"]],
        _data[fields["longitude"]],
        _data[fields["altitude"]],
        _data[fields["temp"]],
        _data[fields["humidity"]],
        _press,
        decimation=skewt_decimation,
    )

    return _output


def calculate_skewt_data(
    datetime,
    latitude,
    longitude,
    altitude,
    temperature,
    humidity,
    pressure=None,
    decimation=5,
):
    """ Work through a set of sonde data, and produce a dataset suitable for plotting in skewt-js """

    # A few basic checks initially

    # Don't bother to plot data with not enough data points.
    if len(datetime) < 10:
        return []

    # Figure out if we have any ascent data at all.
    _burst_idx = np.argmax(altitude)

    if _burst_idx == 0:
        # We only have descent data.
        return []

    if altitude[0] > 15000:
        # No point plotting SkewT plots for data only gathered above 10km altitude...
        return []

    _skewt = []

    i = 0
    while i < _burst_idx:
        i += decimation
        try:
            if temperature[i] < -260.0 or humidity[i] < 0.0:
                # If we don't have any valid temp or humidity data, just skip this point
                # to avoid doing un-necessary calculations
                continue

            _time_delta = (parse(datetime[i]) - parse(datetime[i - 1])).total_seconds()
            if _time_delta == 0:
                continue

            _old_pos = (latitude[i - 1], longitude[i - 1], altitude[i - 1])
            _new_pos = (latitude[i], longitude[i], altitude[i])

            _pos_delta = position_info(_old_pos, _new_pos)

            _speed = _pos_delta["great_circle_distance"] / _time_delta
            _bearing = _pos_delta["bearing"]

            if pressure is None:
                _pressure = getDensity(altitude[i], get_pressure=True) / 100.0
            elif pressure[i] < 0.0:
                _pressure = getDensity(altitude[i], get_pressure=True) / 100.0
            else:
                _pressure = pressure[i]

            _temp = temperature[i]
            _rh = humidity[i]

            _dp = (
                243.04
                * (np.log(_rh / 100) + ((17.625 * _temp) / (243.04 + _temp)))
                / (17.625 - np.log(_rh / 100) - ((17.625 * _temp) / (243.04 + _temp)))
            )

            if np.isnan(_dp):
                continue

            _skewt.append(
                {
                    "press": _pressure,
                    "hght": altitude[i],
                    "temp": _temp,
                    "dwpt": _dp,
                    "wdir": _bearing,
                    "wspd": _speed,
                }
            )

            # Only produce data up to 100hPa, which is the top of the skewt plot.
            if _pressure < 100.0:
                break

        except Exception as e:
            print(str(e))

        # Continue through the data..

    return _skewt


def read_log_by_serial(serial, skewt_decimation=25):
    """ Attempt to read in a log file for a particular sonde serial number """

    # Search in the logging directory for a matching serial number
    _log_mask = os.path.join(autorx.logging_path, f"*_*{serial}_*_sonde.log")
    _matching_files = glob.glob(_log_mask)

    # No matching entries found
    if len(_matching_files) == 0:
        return {}
    else:
        try:
            data = read_log_file(_matching_files[0], skewt_decimation=skewt_decimation)
            return data
        except Exception as e:
            logging.exception(f"Error reading file for serial: {serial}", e)
            return {}


if __name__ == "__main__":
    import sys

    logging.basicConfig(
        format="%(asctime)s %(levelname)s:%(message)s", level=logging.DEBUG
    )

    print(list_log_files())

    if len(sys.argv) > 1:
        print(f"Attempting to read serial: {sys.argv[1]}")
        read_log_by_serial(sys.argv[1])