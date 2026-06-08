#!/usr/bin/env python3
"""
GNSS Receiver Simulator
Simulates two GNSS receivers by sending NMEA GGA and ZDA sentences over UDP.
Useful for testing the GNSS manager without real hardware.

Usage:
    python3 gnss_simulator.py --host localhost --rx1-port 9001 --rx2-port 9002 --interval 1.0

Note: Start this before running the GNSS manager (gnss/app.py)
"""

import socket
import asyncio
import argparse
import logging
import time
import math
from typing import Tuple

logging.basicConfig(level=logging.INFO)
logger = logging.getLogger(__name__)


class GNSSSimulator:
    """Simulates two GNSS receivers sending NMEA sentences"""
    
    def __init__(self, host: str, rx1_port: int, rx2_port: int, interval: float):
        self.host = host
        self.rx1_port = rx1_port
        self.rx2_port = rx2_port
        self.interval = interval
        self.sockets = {}
        self.simulation_time = 0
    
    @staticmethod
    def calculate_checksum(sentence: str) -> str:
        """Calculate NMEA 0183 checksum for a sentence.
        
        Args:
            sentence: NMEA sentence without $ and without checksum (e.g., "GPGGA,123519,...")
        
        Returns:
            Two-digit hexadecimal checksum string
        """
        checksum = 0
        for char in sentence:
            checksum ^= ord(char)
        return f"{checksum:02X}"
    
    def create_sockets(self):
        """Create UDP sockets for both receivers"""
        try:
            sock1 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sock2 = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            self.sockets = {
                1: (sock1, (self.host, self.rx1_port)),
                2: (sock2, (self.host, self.rx2_port))
            }
            logger.info(f"Created sockets for RX1:{self.rx1_port} and RX2:{self.rx2_port}")
        except Exception as e:
            logger.error(f"Socket creation error: {e}")
            raise
    
    @staticmethod
    def decimal_to_nmea(coord: float, is_latitude: bool) -> Tuple[str, str]:
        """Convert decimal coordinate to NMEA format"""
        direction = 'N' if coord >= 0 else 'S'
        if is_latitude:
            direction = 'N' if coord >= 0 else 'S'
        else:
            direction = 'E' if coord >= 0 else 'W'
        
        abs_coord = abs(coord)
        degrees = int(abs_coord)
        minutes = (abs_coord - degrees) * 60
        
        if is_latitude:
            nmea_str = f"{degrees:02d}{minutes:07.4f}"
        else:
            nmea_str = f"{degrees:03d}{minutes:07.4f}"
        
        return nmea_str, direction
    
    def generate_gga_sentence(self, rx_id: int) -> str:
        """Generate NMEA GGA sentence with realistic data and checksum"""
        # Simulate slightly different positions for each receiver
        base_lat = 18.9000 #37.7749
        base_lon = 72.5000 #-122.4194
        
        # Add small offsets and variation
        offset_lat = 0.0001 * math.sin(self.simulation_time + rx_id)
        offset_lon = 0.0001 * math.cos(self.simulation_time + rx_id)
        
        lat = base_lat + offset_lat
        lon = base_lon + offset_lon
        
        lat_nmea, lat_dir = self.decimal_to_nmea(lat, True)
        lon_nmea, lon_dir = self.decimal_to_nmea(lon, False)
        
        # RX1 has better quality (fix=4 RTK), RX2 has standard GPS (fix=1)
        if rx_id == 1:
            fix_quality = 4  # RTK Fixed
            num_sats = 15
            hdop = 0.5
        else:
            fix_quality = 1  # GPS
            num_sats = 12
            hdop = 1.2
        
        altitude = 100.0 + 5 * math.sin(self.simulation_time)
        
        # Format: $GPGGA,time,lat,lat_dir,lon,lon_dir,fix,sats,hdop,alt,M,...
        time_str = time.strftime("%H%M%S.00", time.gmtime())
        
        # Build sentence without $ and checksum
        sentence = (f"GPGGA,{time_str},{lat_nmea},{lat_dir},"
                   f"{lon_nmea},{lon_dir},{fix_quality},{num_sats},"
                   f"{hdop:.1f},{altitude:.1f},M,0.0,M,,")
        
        # Calculate checksum and format complete sentence
        checksum = self.calculate_checksum(sentence)
        gga = f"${sentence}*{checksum}"
        
        return gga
    
    def generate_zda_sentence(self) -> str:
        """Generate NMEA ZDA sentence (Date/Time) with checksum"""
        time_str = time.strftime("%H%M%S.00", time.gmtime())
        date_str = time.strftime("%d,%m,%Y", time.gmtime())
        
        # Build sentence without $ and checksum
        # Format: GPZDA,time,dd,mm,yyyy,xx,yy*hh
        sentence = f"GPZDA,{time_str},{date_str},+00,00"
        
        # Calculate checksum and format complete sentence
        checksum = self.calculate_checksum(sentence)
        zda = f"${sentence}*{checksum}"
        
        return zda
    
    async def send_sentences(self, rx_id: int):
        """Send NMEA sentences from a receiver"""
        sock, addr = self.sockets[rx_id]
        
        try:
            while True:
                # Send GGA sentence
                gga = self.generate_gga_sentence(rx_id)
                sock.sendto(gga.encode(), addr)
                logger.debug(f"[RX{rx_id}] Sent: {gga[:60]}...")
                
                # Wait half interval
                await asyncio.sleep(self.interval / 2)
                
                # Send ZDA sentence
                zda = self.generate_zda_sentence()
                sock.sendto(zda.encode(), addr)
                logger.debug(f"[RX{rx_id}] Sent: {zda}")
                
                # Wait remaining half interval
                await asyncio.sleep(self.interval / 2)
                
                self.simulation_time += self.interval
        
        except Exception as e:
            logger.error(f"[RX{rx_id}] Send error: {e}")
    
    async def run(self):
        """Run the simulator"""
        self.create_sockets()
        
        logger.info(f"Starting GNSS simulator (interval: {self.interval}s)")
        logger.info(f"Broadcasting to {self.host}:{self.rx1_port} and {self.host}:{self.rx2_port}")
        
        try:
            tasks = [
                asyncio.create_task(self.send_sentences(1)),
                asyncio.create_task(self.send_sentences(2))
            ]
            
            await asyncio.gather(*tasks)
        
        except KeyboardInterrupt:
            logger.info("Shutting down simulator")
        finally:
            for sock, _ in self.sockets.values():
                sock.close()


async def main():
    parser = argparse.ArgumentParser(description='GNSS Receiver Simulator')
    parser.add_argument('--host', default='localhost', help='Target host')
    parser.add_argument('--rx1-port', type=int, default=9001, help='RX1 UDP port')
    parser.add_argument('--rx2-port', type=int, default=9002, help='RX2 UDP port')
    parser.add_argument('--interval', type=float, default=1.0, help='Publish interval (seconds)')
    
    args = parser.parse_args()
    
    simulator = GNSSSimulator(args.host, args.rx1_port, args.rx2_port, args.interval)
    await simulator.run()


if __name__ == '__main__':
    asyncio.run(main())
