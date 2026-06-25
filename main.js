console.log("页面加载完成");

const video = document.querySelector("video");

if (video) {
  video.addEventListener("play", () => {
    console.log("视频开始播放");
  });

  video.addEventListener("pause", () => {
    console.log("视频暂停");
  });
}
