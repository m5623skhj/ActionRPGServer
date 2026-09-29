import sys
import tempfile
from pathlib import Path
import unittest
from unittest.mock import patch

import yaml


GENERATOR_DIRECTORY = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(GENERATOR_DIRECTORY))

import PacketGenerator
from PacketSchema import ValidateSchema


class PacketGeneratorTest(unittest.TestCase):
    def test_packet_ids_follow_yaml_order(self):
        protocol = {
            "Namespace": "ActionRPG::DungeonProtocol",
            "PacketIdEnum": "PacketType",
            "RegisterFunction": "RegisterPackets",
        }
        packets, _ = ValidateSchema(
            {
                "Packet": [
                    {"Type": "ReplyPacket", "PacketName": "FirstPacket"},
                    {"Type": "ReplyPacket", "PacketName": "SecondPacket"},
                ]
            }
        )
        header = PacketGenerator.RenderHeader(protocol, packets, "Prerequisite.h")
        self.assertLess(header.index("FIRST_PACKET"), header.index("SECOND_PACKET"))
        self.assertNotIn("= 1001", header)

    def test_generate_and_check_are_reproducible(self):
        with tempfile.TemporaryDirectory() as directory:
            projectRoot = Path(directory)
            schema = projectRoot / "PacketDefine.yml"
            schema.write_text(
                yaml.safe_dump(
                    {
                        "Protocol": {
                            "Namespace": "ActionRPG::DungeonProtocol",
                            "PacketIdEnum": "PacketType",
                            "RegisterFunction": "RegisterPackets",
                            "Targets": [
                                {
                                    "Header": "Generated/DungeonProtocol.h",
                                    "Source": "Generated/DungeonProtocol.cpp",
                                    "PrerequisiteHeader": "Prerequisite.h",
                                }
                            ],
                        },
                        "Packet": [
                            {
                                "Type": "ReplyPacket",
                                "PacketName": "DungeonChallenge",
                                "Items": [{"Type": "std::uint64_t", "Name": "challenge"}],
                            }
                        ],
                    },
                    sort_keys=False,
                ),
                encoding="utf-8",
            )
            with patch.object(PacketGenerator, "PROJECT_ROOT", projectRoot):
                self.assertTrue(PacketGenerator.Generate(schema))
                self.assertTrue(PacketGenerator.Generate(schema, True))
                header = (projectRoot / "Generated" / "DungeonProtocol.h").read_text()
                self.assertIn("DUNGEON_CHALLENGE", header)
                self.assertIn("std::uint64_t challenge", header)

    def test_target_root_can_reference_a_sibling_repository(self):
        with tempfile.TemporaryDirectory() as directory:
            workspace = Path(directory)
            projectRoot = workspace / "Server"
            clientRoot = workspace / "Client"
            projectRoot.mkdir()
            clientRoot.mkdir()
            with patch.object(PacketGenerator, "PROJECT_ROOT", projectRoot):
                output = PacketGenerator.ResolveOutput("../Client", "Generated/Protocol.h")
            self.assertEqual(output, clientRoot / "Generated" / "Protocol.h")

    def test_plain_header_supports_external_packet_field_types(self):
        with tempfile.TemporaryDirectory() as directory:
            projectRoot = Path(directory)
            schema = projectRoot / "TownPacketDefine.yml"
            schema.write_text(
                yaml.safe_dump(
                    {
                        "Protocol": {
                            "Format": "PlainHeader",
                            "Namespace": "TownProtocol",
                            "PacketIdEnum": "PacketType",
                            "Targets": [{"Header": "Generated/TownPacket.generated.h"}],
                        },
                        "ExternalTypes": ["Vector2"],
                        "Packet": [
                            {
                                "Type": "ReplyPacket",
                                "PacketName": "PlayerAppear",
                                "Items": [
                                    {"Type": "std::uint32_t", "Name": "characterId"},
                                    {"Type": "Vector2", "Name": "position"},
                                ],
                            }
                        ],
                    },
                    sort_keys=False,
                ),
                encoding="utf-8",
            )
            with patch.object(PacketGenerator, "PROJECT_ROOT", projectRoot):
                self.assertTrue(PacketGenerator.Generate(schema))
                header = (projectRoot / "Generated" / "TownPacket.generated.h").read_text()
                self.assertIn("PlayerAppear = 1", header)
                self.assertIn("std::uint32_t characterId", header)
                self.assertIn("Vector2 position", header)

    def test_explicit_packet_id_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            schema = Path(directory) / "PacketDefine.yml"
            schema.write_text(
                """Protocol:
  Namespace: Test
  Targets:
    - Header: Generated/Protocol.h
      Source: Generated/Protocol.cpp
      PrerequisiteHeader: Prerequisite.h
Packet:
  - Type: ReplyPacket
    PacketName: Result
    Id: 1001
""",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(ValueError, "list order"):
                PacketGenerator.LoadDefinition(schema)


if __name__ == "__main__":
    unittest.main()
