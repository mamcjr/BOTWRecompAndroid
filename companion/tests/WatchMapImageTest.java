package com.zbotw.recomp;
import java.nio.file.Files;
import java.nio.file.Path;
public class WatchMapImageTest {
 public static void main(String[] args) throws Exception {
  int[] colors = {0xffff0000,0xff00ff00,0xff0000ff,0xffffffff};
  int[] pixels = new int[240*240];
  for(int y=0;y<240;y++) for(int x=0;x<240;x++) pixels[y*240+x]=colors[(y/120)*2+x/120];
  Files.write(Path.of(args[0]), WatchMapImage.encode(pixels));
 }
}
