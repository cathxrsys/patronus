.pragma library

// -----------------------------------------------------------------------------
// Built-in "unique name style" presets.
//
// Each entry is { name, css }, where `css` is the small CSS subset understood by
// NameStyleParser.js. To add a new preset, just append another object here — no
// other code needs to change. The `name` is only a label shown in the picker;
// the `css` is what gets stored, edited and rendered.
// -----------------------------------------------------------------------------

var list = [
    {
        name: "Grad Green",
        css: "color: #d7499a;background: linear-gradient(180deg, #02f80c 50%, #6607a2);color: transparent;-webkit-background-clip: text;text-shadow: 0px 0px 3px #02f80c"
    },
    {
        name: "Aura Red",
        css: "color: black;text-shadow: 0px 0px 3px #ee0b35,0px 0px 3px #ee0b35,0px 0px 3px #ee0b35,0px 0px 3px #ee0b35,0px 0px 3px #ee0b35,0px 0px 3px #ee0b35,0px 0px 3px #ee0b35,0px 0px 3px #ee0b35,0px 0px 3px #ee0b35,0px 0px 3px #ee0b35"
    },
    {
        name: "Aura Purple",
        css: "background: linear-gradient(0deg, #9d4dff, #9500ff); -webkit-background-clip: text; -webkit-text-fill-color: #ffffff1a; color: #ffffff;text-shadow: 0px 3px 3px #9d4dff,0px -2px 1px #9500ff70"
    },
    {
        name: "Aura Aqua",
        css: "color: #00FFFF;text-shadow: -3px 1px 3px #00dcff,3px -1px 3px #00efff,0px 0px 2px #00daff,1px 1px #00719e,0px 0px 3px #00c3e0,0px 0px 1px #00b9ce,1px 1px 1px #009fff,2px 3px 1px #008cad,3px 3px 3px #000a29"
    },
    {
        name: "Aura White",
        css: "color: #000;text-shadow: 0px 1px 3px white, 0px 0px 3px white, 0px 0px 2px white, 1px 1px white, 0px 0px 3px white"
    },
    {
        name: "Aura Purple",
        css: "color: #000;text-shadow: 0px 1px 3px purple, 0px 0px 3px purple, 0px 0px 2px purple, 1px 1px purple, 0px 0px 3px purple"
    },
    {
        name: "Aura Orange XXL",
        css: "color: #FFFFFF;text-shadow: 0px 3px 3px #ff8400,0px -3px 3px #ff8400,-3px 3px 3px #ff8400,-3px 3px 3px #ff8400,3px 3px 3px #ff8400,3px 3px 3px #ff8400,-3px -3px 3px #ff8400,-3px -3px 3px #ff8400,3px -3px 3px #ff8400,3px -3px 3px #ff8400"
    },
    {
        name: "90",
        css: "background: linear-gradient(125deg, #f304eb, #2bdaf7 100%, #00def9); -webkit-background-clip: text; -webkit-text-fill-color: transparent; color: #d0c24d;text-shadow: 3px 3px 3px #006bffc7"
    },
    {
        name: "2021",
        css: "color:rgba(235,230,242,1);text-shadow: 0px 0px 3px #00074b,0px 0px 3px #00d9ff,0px 0px 3px #02133b,0px 0px 3px #0037ff,0px 0px 3px #f08,0px 0px 3px #ff1493,0px 0px 3px #257a99"
    },
    {
        name: "Gray",
        css: "color: rgba(0,0,0,.6); background: #717171; -webkit-background-clip: text; -webkit-text-fill-color: transparent;text-shadow: 3px 2px 3px #fff3,0px 0px 3px #ffffff80,0px 0px 3px #ffffff80,0px -1px 1px #656565,0px -1px #929292"
    },
    {
        name: "Lux",
        css: "background: linear-gradient(20deg, #006eff, #00ff81 52%, #fff 50%, #93cbff);-webkit-background-clip: text;-webkit-text-fill-color: transparent;text-shadow: 0 0 3px rgba(0, 255, 207, .5)"
    },
    {
        name: "Lime",
        css: "color:rgba(157,255,0,1)"
    },
    {
        name: "Secure",
        css: "background: linear-gradient(10deg, #68fb7b, #68fb7b, #68fb7b) text;-webkit-background-clip: text;-webkit-text-fill-color: transparent;text-shadow: 0px 0px 3px #68fb7b"
    },
    {
        name: "Sunset",
        css: "background:linear-gradient(130deg, #CBEE15 0%, #CBEE15 15%, orange 15%, orange 30%, yellow 30%, yellow 45%, #FBFA41 45%, #FBFA41 60%, #EDC510 60%, #EDC510 75%, pink 75%, pink 100%);color:transparent;-webkit-background-clip:text;text-shadow: 0px -1px 3px #e8c924"
    },
    {
        name: "Angel",
        css: "color: #fff;text-shadow: 0px 1px 3px #eaeaea, 0px 0px 3px #eaeaea, 0px 0px 2px #e4e4e4, 1px 1px #f1f1f1, 0px 0px 3px #cacaca"
    },
    {
        name: "Fire",
        css: "background: linear-gradient(80deg, #FF2E00 50%, #EF9E00 1% );-webkit-background-clip: text;-webkit-text-fill-color: transparent;text-shadow: 1px 3px 3px #ffc14721,3px 1px 3px #ec4e0b73"
    },
    {
        name: "Fire Smooth",
        css: "background: linear-gradient(80deg, #FF2E00 1%, #EF9E00 50% );-webkit-background-clip: text;-webkit-text-fill-color: transparent;text-shadow: 1px 3px 3px #ffc14721,3px 1px 3px #ec4e0b73"
    },
    {
        name: "Sky",
        css: "color: #01fdff;text-shadow: 0px 0px 1px #009fdc,0px 0px 3px #009fdc,0px 0px 3px #009fdc,0px 0px 3px #0099d4"
    },
    {
        name: "Vampire",
        css: "color:black;text-shadow: 1px -2px 3px #a81011,1px -1px 3px #f00,-3px 1px 1px #100,0px -1px 1px #f00,0px 2px 2px #000,0px 2px 3px #f00,0px 3px 3px #000,0px 3px 3px #000,0px 2px 1px #000,0px -3px 3px #000"
    },
    {
        name: "Malware",
        css: "color:rgba(255,0,0,1);text-shadow: 0px 0px 3px #8b0000,0px 0px 3px #8b0000,0px 0px 3px #8b0000,0px 0px 3px #8b0000,0px 0px 3px #8b0000,0px 0px 3px #8b0000,0px 0px 3px #8b0000,0px 0px 3px #8b0000"
    },
    {
        name: "Dead",
        css: "background:#C6DEE3 100%;color:transparent;-webkit-background-clip:text;text-shadow: 1px 1px 3px #000,0px 0px 3px #000"
    },
    {
        name: "Pink",
        css: "color: rgb(255, 20, 147);"
    },
    {
        name: "Reaper",
        css: "color: rgb(0, 0, 0); text-shadow: rgb(255, 106, 0) 0px 0px 3px, rgb(255, 106, 0) 0px -2px 3px, rgb(255, 106, 0) 3px 0px 3px, rgb(255, 106, 0) -3px -2px 3px, rgb(255, 106, 0) 0px -3px 3px;"
    },
    {
        name: "Amethist",
        css: "background: linear-gradient(20deg, rgb(228, 228, 228) 1%, rgb(251, 250, 65) 60%) text; -webkit-text-fill-color: transparent; color: rgb(228, 228, 228); text-shadow: rgba(228, 228, 228, 0.44) 0px 0px 3px;"
    },
    {
        name: "Demon Eye",
        css: "background: linear-gradient(45deg, rgb(255, 45, 149) 50%, rgb(2, 248, 12) 52%, rgb(255, 132, 0)) text; -webkit-text-fill-color: transparent; color: rgb(255, 45, 149); text-shadow: rgba(255, 45, 149, 0.5) 0px 0px 3px;"
    },
    {
        name: "Platinum Gold",
        css: "background: linear-gradient(rgb(255, 230, 0) 1%, rgb(255, 106, 0) 100%) text; -webkit-text-fill-color: transparent; color: rgb(255, 230, 0); text-shadow: rgba(255, 115, 0, 0.44) 0px 0px 3px;"
    },
    {
        name: "Nature",
        css: "background: linear-gradient(10deg, rgb(255, 230, 0) 1%, rgb(0, 229, 255) 52%) text; -webkit-text-fill-color: transparent; color: rgb(255, 230, 0); text-shadow: rgba(0, 195, 224, 0.44) 0px 0px 3px;"
    },
    {
        name: "Nebula",
        css: "color: rgb(0, 0, 0); text-shadow: rgb(0, 110, 255) -2px 2px 3px, rgb(0, 110, 255) 0px 1px 3px, rgb(0, 110, 255) 2px 0px 3px, rgb(0, 110, 255) 1px -3px 3px, rgb(0, 110, 255) -3px -3px 3px, rgb(0, 110, 255) 2px -2px 3px;"
    },
    {
        name: "Retro Wave",
        css: "color: rgb(215, 73, 154);"
    },
    {
        name: "Cyberpunk",
        css: "background: linear-gradient(60deg, rgb(0, 195, 224) 50%, rgb(0, 255, 129)) text; -webkit-text-fill-color: transparent; color: rgb(0, 195, 224); text-shadow: rgba(251, 250, 65, 0.44) 0px 0px 3px;"
    },
    {
        name: "Rogue",
        css: "color: rgb(26, 26, 26); text-shadow: rgb(238, 11, 53) 0px 0px 3px, rgb(238, 11, 53) 0px 0px 3px;"
    },
    {
        name: "Crystal",
        css: "color: rgb(10, 10, 10); text-shadow: rgb(1, 253, 255) 3px -3px 3px, rgb(1, 253, 255) 0px 0px 3px, rgb(1, 253, 255) -3px 1px 3px, rgb(1, 253, 255) 2px 2px 3px;"
    },
    {
        name: "Glitch",
        css: "color: rgb(0, 0, 0); text-shadow: rgb(160, 32, 240) -1px -3px 3px, rgb(160, 32, 240) -3px -2px 3px, rgb(160, 32, 240) 2px 3px 3px, rgb(160, 32, 240) 3px 1px 3px, rgb(160, 32, 240) 1px -3px 3px, rgb(160, 32, 240) 3px 2px 3px, rgb(160, 32, 240) 3px -1px 3px, rgb(160, 32, 240) 1px -3px 3px;"
    },
    {
        name: "Barbie",
        css: "color: rgb(233, 30, 99); text-shadow: rgb(233, 30, 99) -2px -2px 3px, rgb(10, 10, 10) 1px -2px 3px, rgb(233, 30, 99) -2px 1px 3px, rgb(10, 10, 10) 0px 3px 3px, rgb(233, 30, 99) 0px -1px 3px, rgb(10, 10, 10) 3px -1px 3px, rgb(233, 30, 99) -3px -3px 3px, rgb(10, 10, 10) 0px 2px 3px, rgb(233, 30, 99) 3px 3px 3px;"
    },
    {
        name: "Noble Vector",
        css: "color: rgb(0, 0, 0); text-shadow: rgb(198, 0, 0) -2px 1px 3px, rgb(157, 255, 0) 3px 1px 3px, rgb(198, 0, 0) -1px -1px 3px, rgb(157, 255, 0) 3px 3px 3px;"
    },
    {
        name: "Ancient",
        css: "color: rgb(255, 255, 255); text-shadow: rgb(21, 101, 192) -2px 1px 3px, rgb(26, 26, 26) 2px -3px 3px, rgb(21, 101, 192) -3px -3px 3px, rgb(26, 26, 26) 0px 3px 3px, rgb(21, 101, 192) 3px 3px 3px, rgb(26, 26, 26) 2px 0px 3px, rgb(21, 101, 192) -3px 0px 3px;"
    },
    {
        name: "Cobra",
        css: "color: rgb(0, 0, 0); text-shadow: rgb(178, 34, 34) 0px 2px 3px, rgb(225, 190, 231) 3px -1px 3px, rgb(178, 34, 34) -1px 3px 3px, rgb(225, 190, 231) -1px -1px 3px;"
    },
    {
        name: "Broken",
        css: "color: rgb(0, 0, 0); text-shadow: rgb(57, 255, 20) -3px 1px 3px, rgb(158, 158, 158) 0px 3px 3px, rgb(57, 255, 20) 1px -3px 3px, rgb(158, 158, 158) 2px -3px 3px;"
    },
    {
        name: "Hour",
        css: "color: rgb(255, 0, 136); text-shadow: rgb(255, 182, 193) 0px 0px 3px, rgb(255, 182, 193) 0px 0px 3px, rgb(255, 182, 193) 0px 0px 3px, rgb(255, 182, 193) 0px 0px 3px, rgb(255, 182, 193) 0px 0px 3px, rgb(255, 182, 193) 0px 0px 3px;"
    },
    {
        name: "Error",
        css: "color: rgb(255, 255, 255); text-shadow: rgb(0, 255, 129) 0px -1px 3px, rgb(103, 58, 183) 3px 1px 3px, rgb(0, 255, 129) 1px -3px 3px, rgb(103, 58, 183) -1px -1px 3px, rgb(0, 255, 129) 0px 2px 3px, rgb(103, 58, 183) 3px -1px 3px, rgb(0, 255, 129) 2px -3px 3px, rgb(103, 58, 183) 0px 3px 3px;"
    },
    {
        name: "Pulse",
        css: "color: rgb(255, 255, 255); text-shadow: rgb(255, 0, 136) 1px -3px 3px, rgb(179, 229, 252) -1px 3px 3px, rgb(255, 0, 136) 0px 3px 3px, rgb(179, 229, 252) 0px -1px 3px, rgb(255, 0, 136) -3px 1px 3px, rgb(179, 229, 252) 2px 0px 3px, rgb(255, 0, 136) 3px -1px 3px, rgb(179, 229, 252) -2px -2px 3px, rgb(255, 0, 136) 2px 3px 3px;"
    },
    {
        name: "Ambulance",
        css: "color: rgb(220, 20, 60); text-shadow: rgb(209, 196, 233) 0px 0px 3px, rgb(209, 196, 233) 0px 0px 3px, rgb(209, 196, 233) 0px 0px 3px, rgb(209, 196, 233) 0px 0px 3px, rgb(209, 196, 233) 0px 0px 3px;"
    },
    {
        name: "Reef",
        css: "color: rgb(255, 249, 196); text-shadow: rgb(255, 236, 179) 0px 1px 3px, rgb(255, 236, 179) 0px 0px 2px, rgb(255, 236, 179) 1px 1px;"
    },
    {
        name: "Chill",
        css: "color: rgb(255, 255, 255); text-shadow: rgb(255, 209, 220) -2px 1px 3px, rgb(139, 69, 19) -2px -2px 3px, rgb(255, 209, 220) -1px 3px 3px, rgb(139, 69, 19) 3px -1px 3px;"
    },
    {
        name: "Howl",
        css: "color: rgb(26, 26, 26); text-shadow: rgb(200, 230, 255) 0px 0px 3px, rgb(200, 230, 255) 0px 2px 3px, rgb(200, 230, 255) -1px -1px 3px;"
    },
    {
        name: "Onyx",
        css: "color: rgb(255, 255, 255); text-shadow: rgb(225, 190, 231) -3px -3px 3px, rgb(255, 77, 148) 3px 3px 3px, rgb(225, 190, 231) -2px -2px 3px, rgb(255, 77, 148) 0px -1px 3px, rgb(225, 190, 231) 2px 3px 3px, rgb(255, 77, 148) -3px 0px 3px, rgb(225, 190, 231) -3px 1px 3px;"
    },
    {
        name: "Mystic",
        css: "color: rgb(0, 0, 0); text-shadow: rgb(26, 188, 156) 1px 1px 3px, rgb(26, 188, 156) 3px 3px 3px, rgb(26, 188, 156) -2px 1px 3px, rgb(26, 188, 156) 2px -3px 3px;"
    },
    {
        name: "80",
        css: "background: linear-gradient(0deg, rgb(0, 153, 212) 50%, rgb(255, 0, 255) 55%) text; -webkit-text-fill-color: transparent; color: rgb(0, 153, 212); text-shadow: rgba(243, 4, 235, 0.5) 0px 0px 3px;"
    },
    {
        name: "Shadow",
        css: "color: rgb(255, 255, 255); text-shadow: rgb(13, 13, 13) 3px -1px 3px, rgb(192, 192, 192) -3px 1px 3px, rgb(13, 13, 13) -2px -2px 3px, rgb(192, 192, 192) 3px 3px 3px, rgb(13, 13, 13) -1px -1px 3px, rgb(192, 192, 192) 1px -3px 3px, rgb(13, 13, 13) -3px -3px 3px, rgb(192, 192, 192) 2px 3px 3px, rgb(13, 13, 13) 0px -1px 3px;"
    },
    {
        name: "Aurora",
        css: "color: rgb(0, 0, 0); text-shadow: rgb(111, 78, 55) 0px 3px 3px, rgb(212, 175, 55) -3px 1px 3px, rgb(111, 78, 55) -3px 0px 3px, rgb(212, 175, 55) 2px 3px 3px, rgb(111, 78, 55) 3px -1px 3px, rgb(212, 175, 55) 3px 3px 3px;"
    },
    {
        name: "Cursed Flow",
        css: "background: linear-gradient(45deg, rgb(236, 0, 140) 0%, rgb(236, 0, 140) 40%, rgb(178, 34, 34) 40%, rgb(178, 34, 34) 100%) text; color: transparent; text-shadow: rgb(236, 0, 140) 0px 0px 3px;"
    },
    {
        name: "Silent",
        css: "color: rgb(13, 13, 13); text-shadow: rgb(255, 105, 180) 0px 3px 3px, rgb(255, 105, 180) 2px -3px 3px, rgb(255, 105, 180) 0px 0px 3px;"
    },
    {
        name: "Analog",
        css: "color: rgb(255, 236, 179); text-shadow: rgb(224, 0, 48) 0px 0px 3px, rgb(224, 0, 48) 0px 0px 3px, rgb(224, 0, 48) 0px 0px 3px, rgb(224, 0, 48) 0px 0px 3px;"
    },
    {
        name: "Shard",
        css: "color: rgb(0, 0, 0); text-shadow: rgb(255, 20, 147) -2px 1px 3px, rgb(0, 159, 220) -3px -3px 3px, rgb(255, 20, 147) 1px 1px 3px, rgb(0, 159, 220) 1px -3px 3px, rgb(255, 20, 147) 2px 3px 3px, rgb(0, 159, 220) -1px -1px 3px;"
    },
    {
        name: "Fallen",
        css: "color: rgb(0, 0, 0); text-shadow: rgb(26, 26, 26) 3px 1px 3px, rgb(255, 77, 148) -3px 0px 3px, rgb(26, 26, 26) 1px 1px 3px, rgb(255, 77, 148) 2px -3px 3px, rgb(26, 26, 26) 1px -2px 3px;"
    },
];

