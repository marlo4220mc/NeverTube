const https=require("https"),fs=require("fs");
const KEY="AIzaSyAO_FJ2SlqU8Q4STEHLGCilw_Y9_11qcW8";
const UA="com.google.android.youtube/21.26.364 (Linux; U; Android 11) gzip";
const b=JSON.stringify({context:{client:{clientName:"ANDROID",clientVersion:"21.26.364",hl:"en",gl:"US",userAgent:UA,androidSdkVersion:30,osName:"Android",osVersion:"11"}},videoId:"7wVjjtpWWZs"});
const q=https.request({host:"www.youtube.com",path:"/youtubei/v1/player?key="+KEY+"&prettyPrint=false",method:"POST",headers:{"Content-Type":"application/json","Content-Length":Buffer.byteLength(b),"User-Agent":UA}},r=>{let d="";r.on("data",x=>d+=x);r.on("end",()=>{
 const j=JSON.parse(d); const sd=j.streamingData||{};
 const f=(sd.formats||[])[0];
 if(f) console.log("  formats[0] (itag "+f.itag+"): sabr="+("sabr=1" in f.url)+" ratebypass="+("ratebypass" in f.url)+" gir="+("gir=yes" in f.url));
 console.log("  serverAbrStreamingUrl (SABR):", sd.serverAbrStreamingUrl?("SI, "+sd.serverAbrStreamingUrl.length+" chars"):"no");
});});
q.write(b);q.end();
